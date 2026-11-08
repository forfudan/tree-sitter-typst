// **Markup is lexed here; so is everything Typst decides by looking around.**
//
// Typst's lexer (`crates/typst-syntax/src/lexer.rs`) looks back one character
// to decide whether `*` sits inside a word, looks at the start of the line to
// decide whether `-` is a list marker, and its parser (`parser.rs`) ends a list
// item at the first line indented no deeper than the marker. None of that is a
// regular expression, so markup is scanned here in full, and the code-mode
// tokens whose meaning depends on whitespace come from here as well.
//
// ---- The contract with tree-sitter ------------------------------------------
//
// Read off `lib/src/lexer.c` and `lib/src/parser.c` (tree-sitter 0.25):
//
// 1. This function is called before the internal lexer whenever the parse state
//    has an external token to offer. Returning false resets the position and
//    lets the internal lexer try from the same byte.
// 2. `advance(lexer, true)` moves the token start along with the position: the
//    character becomes padding. `advance(lexer, false)` makes it part of the
//    token. Once `mark_end` has been called, only `advance(lexer, false)` is
//    safe, or the start would pass the end.
// 3. `mark_end` fixes the token's end where the position is now; the last call
//    wins, so a scan may run ahead to decide and still hand back a shorter
//    token, or a zero-width one.
// 4. The next scan starts at the token's end, not where scanning stopped. A
//    zero-width token placed before a line break is how one line break can end
//    a heading, three list items and then still be markup whitespace: every
//    call crosses the same line break again and pops one more thing.
// 5. A zero-width token is dropped when the serialized state did not change and
//    the parser is recovering or has not advanced since an error. `_item_end`
//    always pops a column, so it changes the state. `_newline`, `_line_end` and
//    `_heading_end` are never accepted twice in a row, and `_unclosed` twice
//    only for two different delimiters, so none of them is asked for forever.
// 6. During error recovery every symbol is valid. `_error_sentinel` is never
//    valid in a real state, so seeing it valid means "recovering".
// 7. The state is serialized after every external token and restored before
//    every call, from the last external token on that stack. Internal tokens do
//    not touch it. This is why markup is external all the way down: the flags
//    below ("at the start of a line", "a space came first") are only true if
//    every markup token passes through here and resets them.
//
// ---- How the scanner tells the modes apart ----------------------------------
//
// tree-sitter never merges two parse states when that would make an external
// token valid where it was not, so `valid[TEXT]` means exactly "markup may
// continue here": in markup, and right after an expression embedded with `#`.
// Without it the state is code or math, and whitespace is padding.
// `valid[CALL_PAREN]` means exactly "an expression has just ended", since any
// expression may be called. `valid[NEWLINE]` means a statement in a code block
// may end, where the next line may still go on with `.` or `else`;
// `valid[LINE_END]` means a statement after a hash may end, where it may not.

#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"
#include "unicode.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

enum TokenType {
  SPACE,
  PARBREAK,
  BLOCK_COMMENT,
  NEWLINE,
  ITEM_END,
  HEADING_END,
  TEXT,
  HASH,
  DOLLAR,
  CONTENT_OPEN,
  CALL_BRACKET,
  TEXT_LBRACKET,
  RBRACKET,
  CALL_PAREN,
  FIELD_DOT,
  ELSE,
  STAR,
  UNDERSCORE,
  HEADING_MARKER,
  LIST_MARKER,
  ENUM_MARKER,
  TERM_MARKER,
  TERM_COLON,
  ESCAPE,
  LINEBREAK,
  SHORTHAND,
  URL,
  LABEL,
  REF_MARKER,
  RAW_OPEN_INLINE,
  RAW_OPEN_BLOCK,
  RAW_LANG,
  RAW_TEXT,
  RAW_CLOSE,
  MATH_ARG_NAME,
  TRAILING_DOT_FLOAT,
  STRING_CONTENT,
  LINE_END,
  UNCLOSED,
  ERROR_SENTINEL,
};

typedef struct {
  // Backticks that opened the raw text we are inside, 0 outside raw text.
  uint16_t raw_ticks;
  // Only whitespace (and comments) since the last line break: a marker here
  // starts a heading or an item.
  bool line_start;
  // Right after `[`, a list or enum marker or a term's colon: Typst parses the
  // markup that follows "at start" too, so `[- a]` is a list.
  bool block_start;
  // The token before was markup whitespace or a comment, so a `[`, `(` or `.`
  // here does not attach to the expression before it.
  bool after_space;
  // A block comment just took the line break a statement would have ended at,
  // so the scan after it still counts that line break.
  bool pending_newline;
  // A block comment in code was followed straight away by `(` or `[`, which
  // therefore does not call what came before the comment (`directly_at`). The
  // comment looks at the next character itself, so no token comes between;
  // the column of that character makes it one-shot, since internal tokens
  // after it cannot clear the flag.
  bool comment_then_open;
  uint16_t comment_then_open_col;
  // Column of every open list, enum and term item's marker, innermost last.
  Array(uint16_t) columns;
} Scanner;

// ---- characters -------------------------------------------------------------

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }
static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static inline bool is_newline(int32_t c) {
  return c == '\n' || c == '\r' || c == 0x0B || c == 0x0C || c == 0x85 || c == 0x2028 ||
         c == 0x2029;
}

// Whitespace in markup is only spaces, tabs and line breaks; a no-break space
// there is text (`is_space` in `lexer.rs`). Code and math take all of Unicode's.
static inline bool is_markup_space(int32_t c) { return c == ' ' || c == '\t' || is_newline(c); }

static inline bool is_space(int32_t c) {
  return c == ' ' || c == '\t' || is_newline(c) || c == 0xA0 || c == 0x1680 ||
         (c >= 0x2000 && c <= 0x200A) || c == 0x202F || c == 0x205F || c == 0x3000 ||
         c == 0xFEFF;
}

// East Asian scripts do not form words for `*` and `_` (`in_word` in
// `lexer.rs`): `中文*强调*` is strong.
static inline bool is_cjk(int32_t c) {
  return (c >= 0x2E80 && c <= 0x2FDF) || (c >= 0x3040 && c <= 0x30FF) ||
         (c >= 0x3100 && c <= 0x312F) || (c >= 0x3130 && c <= 0x318F) ||
         (c >= 0x31A0 && c <= 0x31FF) || (c >= 0x3400 && c <= 0x4DBF) ||
         (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA960 && c <= 0xA97F) ||
         (c >= 0xAC00 && c <= 0xD7FF) || (c >= 0xF900 && c <= 0xFAFF) ||
         (c >= 0xFF66 && c <= 0xFFDC) || (c >= 0x20000 && c <= 0x3FFFF);
}

// Rust's `char::is_alphanumeric`, which `in_word` and smart text ask.
static inline bool is_alnum(int32_t c) {
  if (c < 0x80) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  }
  return TYPST_IN(typst_alnum, c);
}

// Rust's `char::is_numeric`: a hyphen before one is a minus sign.
static inline bool is_numeric(int32_t c) {
  if (c < 0x80) return c >= '0' && c <= '9';
  return TYPST_IN(typst_numeric, c);
}

static inline bool is_wordy(int32_t c) { return is_alnum(c) && !is_cjk(c); }

// `is_id_start` and `is_id_continue` in `lexer.rs`: Unicode identifiers, plus
// `_` anywhere and `-` after the first character.
static inline bool is_id_start(int32_t c) {
  if (c < 0x80) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
  return TYPST_IN(typst_xid_start, c);
}

static inline bool is_id_continue(int32_t c) {
  if (c < 0x80) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '_' || c == '-';
  }
  return TYPST_IN(typst_xid_continue, c);
}

// After a `.`: does a field name follow? `_` alone is not a name (`ident` in
// `lexer.rs` makes it an underscore), so `#title._` is not a field access.
static bool at_field_name(TSLexer *lexer) {
  if (lexer->lookahead == '_') {
    advance(lexer);
    return is_id_continue(lexer->lookahead);
  }
  return is_id_start(lexer->lookahead);
}

static inline bool is_label_char(int32_t c) { return is_id_continue(c) || c == ':' || c == '.'; }

// `link_prefix` in `lexer.rs`.
static inline bool is_url_char(int32_t c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         strchr("!#$%&*+,-./:;=?@_~'", c) != NULL;
}

static inline bool is_url_trailing(int32_t c) { return c && strchr("!,.:;?'", c) != NULL; }

// ---- state ------------------------------------------------------------------

static void reset(Scanner *s) {
  s->raw_ticks = 0;
  s->line_start = true;
  s->block_start = false;
  s->after_space = false;
  s->pending_newline = false;
  s->comment_then_open = false;
  s->comment_then_open_col = 0;
  array_clear(&s->columns);
}

void *tree_sitter_typst_external_scanner_create(void) {
  Scanner *s = ts_calloc(1, sizeof(Scanner));
  array_init(&s->columns);
  reset(s);
  return s;
}

void tree_sitter_typst_external_scanner_destroy(void *payload) {
  Scanner *s = payload;
  array_delete(&s->columns);
  ts_free(s);
}

unsigned tree_sitter_typst_external_scanner_serialize(void *payload, char *buffer) {
  Scanner *s = payload;
  unsigned n = 0;
  buffer[n++] = (char)(s->raw_ticks & 0xFF);
  buffer[n++] = (char)(s->raw_ticks >> 8);
  buffer[n++] = (char)((s->line_start ? 1 : 0) | (s->block_start ? 2 : 0) |
                       (s->after_space ? 4 : 0) | (s->pending_newline ? 8 : 0) |
                       (s->comment_then_open ? 16 : 0));
  buffer[n++] = (char)(s->comment_then_open_col & 0xFF);
  buffer[n++] = (char)(s->comment_then_open_col >> 8);
  for (uint32_t i = 0; i < s->columns.size; i++) {
    if (n + 2 > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) break;
    uint16_t col = *array_get(&s->columns, i);
    buffer[n++] = (char)(col & 0xFF);
    buffer[n++] = (char)(col >> 8);
  }
  return n;
}

void tree_sitter_typst_external_scanner_deserialize(void *payload, const char *buffer,
                                                    unsigned length) {
  Scanner *s = payload;
  reset(s);
  if (length < 5) return;
  s->raw_ticks = (uint16_t)((uint8_t)buffer[0] | ((uint8_t)buffer[1] << 8));
  uint8_t flags = (uint8_t)buffer[2];
  s->line_start = flags & 1;
  s->block_start = flags & 2;
  s->after_space = flags & 4;
  s->pending_newline = flags & 8;
  s->comment_then_open = flags & 16;
  s->comment_then_open_col = (uint16_t)((uint8_t)buffer[3] | ((uint8_t)buffer[4] << 8));
  for (unsigned i = 5; i + 1 < length; i += 2) {
    array_push(&s->columns, (uint16_t)((uint8_t)buffer[i] | ((uint8_t)buffer[i + 1] << 8)));
  }
}

// ---- emitting ---------------------------------------------------------------

// A token that is not trivia: the next one is no longer at the start of a line
// or block, nor after a space.
static bool emit(Scanner *s, TSLexer *lexer, enum TokenType type) {
  lexer->result_symbol = type;
  s->line_start = false;
  s->block_start = false;
  s->after_space = false;
  s->pending_newline = false;
  s->comment_then_open = false;
  return true;
}

// `[` and the markers after which markup is parsed "at start" again.
static bool emit_opening(Scanner *s, TSLexer *lexer, enum TokenType type) {
  emit(s, lexer, type);
  s->block_start = true;
  return true;
}

static bool emit_item_marker(Scanner *s, TSLexer *lexer, enum TokenType type, uint32_t col) {
  array_push(&s->columns, (uint16_t)(col > 0xFFFF ? 0xFFFF : col));
  return emit_opening(s, lexer, type);
}

// A zero-width token leaves every flag as it was.
static bool emit_zero(TSLexer *lexer, enum TokenType type) {
  lexer->result_symbol = type;
  return true;
}

// The zero-width tokens that close markup structures. `line_start` says the
// position stands on the first token of a line.
//
// Warning: **ask for the column only here, and only at the start of a line.**
// `get_column` walks back to the line's start and counts forward, and it marks
// the token as depending on its column, which costs incremental reuse.
//
// `col` is the line's column when the caller already had to look; -1 asks here.
static bool close_structure(Scanner *s, TSLexer *lexer, const bool *valid, bool line_start,
                            bool at_end, int64_t col) {
  // A strong or emphasis still open where its block ends.
  if (valid[UNCLOSED] && at_end) return emit_zero(lexer, UNCLOSED);
  if (valid[ITEM_END]) {
    if (s->columns.size == 0) {
      if (at_end || line_start) return emit_zero(lexer, ITEM_END);
    } else if (at_end ||
               (line_start &&
                (col >= 0 ? col : (int64_t)lexer->get_column(lexer)) <= *array_back(&s->columns))) {
      array_pop(&s->columns);
      return emit_zero(lexer, ITEM_END);
    }
  }
  if (valid[HEADING_END] && (at_end || line_start)) return emit_zero(lexer, HEADING_END);
  return false;
}

// A statement ends: zero width, before the whitespace it was found in.
static bool emit_newline(Scanner *s, TSLexer *lexer) {
  s->pending_newline = false;
  s->comment_then_open = false;
  return emit_zero(lexer, NEWLINE);
}

// A block comment in code: remember whether `(` or `[` follows it at once.
static bool emit_code_comment(Scanner *s, TSLexer *lexer) {
  lexer->result_symbol = BLOCK_COMMENT;
  s->comment_then_open = lexer->lookahead == '(' || lexer->lookahead == '[';
  if (s->comment_then_open) {
    uint32_t col = lexer->get_column(lexer);
    s->comment_then_open_col = (uint16_t)(col > 0xFFFF ? 0xFFFF : col);
  }
  return true;
}

// ---- comments and raw text --------------------------------------------------

// After the opening `/*`. Nested comments count (`block_comment` in `lexer.rs`).
// Moves past the comment without touching the token's end.
static void pass_block_comment(TSLexer *lexer) {
  int depth = 1;
  int32_t prev = 0;
  while (!lexer->eof(lexer)) {
    int32_t c = lexer->lookahead;
    advance(lexer);
    if (prev == '*' && c == '/') {
      if (--depth == 0) break;
      prev = 0;
    } else if (prev == '/' && c == '*') {
      depth++;
      prev = 0;
    } else {
      prev = c;
    }
  }
}

static void finish_block_comment(TSLexer *lexer) {
  pass_block_comment(lexer);
  lexer->mark_end(lexer);
}

// At a backtick. Two backticks are an empty raw span (`raw` in `lexer.rs`),
// three or more open a block.
static bool scan_raw_open(Scanner *s, TSLexer *lexer, const bool *valid) {
  uint32_t ticks = 0;
  while (lexer->lookahead == '`') {
    advance(lexer);
    if (++ticks == 1) lexer->mark_end(lexer);
  }
  if (ticks == 2) {
    ticks = 1;  // the end stays after the first backtick
  } else {
    lexer->mark_end(lexer);
  }
  enum TokenType type = ticks >= 3 ? RAW_OPEN_BLOCK : RAW_OPEN_INLINE;
  if (!valid[type]) return false;
  emit(s, lexer, type);
  s->raw_ticks = (uint16_t)(ticks > 0xFFFF ? 0xFFFF : ticks);
  return true;
}

static bool scan_raw_inside(Scanner *s, TSLexer *lexer, const bool *valid) {
  if (valid[RAW_LANG] && s->raw_ticks >= 3 && is_id_start(lexer->lookahead) &&
      lexer->lookahead != '-') {
    while (is_id_continue(lexer->lookahead)) advance(lexer);
    lexer->mark_end(lexer);
    lexer->result_symbol = RAW_LANG;
    return true;
  }
  // The text runs to the first run of exactly as many backticks as opened it;
  // a longer run closes too and leaves the rest over (`raw` in `lexer.rs`).
  bool content = false;
  uint32_t found = 0;
  lexer->mark_end(lexer);
  while (!lexer->eof(lexer)) {
    if (lexer->lookahead == '`') {
      if (found == 0) lexer->mark_end(lexer);
      advance(lexer);
      if (++found == s->raw_ticks) break;
    } else {
      found = 0;
      content = true;
      advance(lexer);
      lexer->mark_end(lexer);
    }
  }
  if (found == s->raw_ticks && !content) {
    // Nothing between: this is the closing run.
    if (!valid[RAW_CLOSE]) return false;
    lexer->mark_end(lexer);
    emit(s, lexer, RAW_CLOSE);
    s->raw_ticks = 0;
    return true;
  }
  if (lexer->eof(lexer) && found != s->raw_ticks) lexer->mark_end(lexer);
  if (!content || !valid[RAW_TEXT]) return false;
  lexer->result_symbol = RAW_TEXT;
  return true;
}

// ---- code -------------------------------------------------------------------

// After a line break in a code block: does the next line go on with the
// statement (`AtNewline::ContextualContinue`: a `.field` or an `else`)? Line
// comments in between are skipped; a block comment says no.
static bool continues_statement(TSLexer *lexer) {
  for (;;) {
    while (is_space(lexer->lookahead)) advance(lexer);
    if (lexer->lookahead != '/') break;
    advance(lexer);
    if (lexer->lookahead == '/') {
      while (!lexer->eof(lexer) && !is_newline(lexer->lookahead)) advance(lexer);
    } else if (lexer->lookahead == '*') {
      advance(lexer);
      pass_block_comment(lexer);
    } else {
      return false;
    }
  }
  if (lexer->lookahead == '.') {
    advance(lexer);
    return is_id_start(lexer->lookahead);
  }
  if (lexer->lookahead == 'e') {
    const char *word = "else";
    for (int i = 0; word[i]; i++) {
      if (lexer->lookahead != word[i]) return false;
      advance(lexer);
    }
    return !is_id_continue(lexer->lookahead);
  }
  return false;
}

// Consumes `else` if it is there as a whole word.
static bool take_else(TSLexer *lexer) {
  const char *word = "else";
  for (int i = 0; word[i]; i++) {
    if (lexer->lookahead != word[i]) return false;
    advance(lexer);
  }
  return !is_id_continue(lexer->lookahead);
}

static bool scan_code(Scanner *s, TSLexer *lexer, const bool *valid) {
  // A statement after a hash ends at the line break, whatever the next line
  // says, or at the `]` of the block it sits in.
  if (valid[LINE_END] && !valid[NEWLINE] &&
      (is_space(lexer->lookahead) || lexer->eof(lexer) || lexer->lookahead == ']')) {
    lexer->mark_end(lexer);
    bool newline = false, spaced = false;
    while (is_space(lexer->lookahead)) {
      if (is_newline(lexer->lookahead)) newline = true;
      spaced = true;
      advance(lexer);
    }
    if (newline || lexer->eof(lexer) || lexer->lookahead == ']') {
      s->comment_then_open = false;
      return emit_zero(lexer, LINE_END);
    }
    if (spaced) {
      lexer->mark_end(lexer);
      lexer->result_symbol = SPACE;
      return true;
    }
  }

  // A statement ends here? The token sits before the whitespace, so the next
  // scan crosses the same line break and markup after it starts a line.
  if (valid[NEWLINE] && (is_space(lexer->lookahead) || lexer->eof(lexer) ||
                         lexer->lookahead == ']' || s->pending_newline)) {
    lexer->mark_end(lexer);
    bool newline = s->pending_newline, spaced = false;
    while (is_space(lexer->lookahead)) {
      if (is_newline(lexer->lookahead)) newline = true;
      spaced = true;
      advance(lexer);
    }
    if (lexer->eof(lexer) || lexer->lookahead == ']') return emit_newline(s, lexer);
    if (newline && s->pending_newline) {
      // Right after a block comment that took a line break along. Spaces
      // first are a token of their own, so that the continuation, if there
      // is one, is a token of ours too and the remembered line break ends.
      if (spaced) {
        lexer->mark_end(lexer);
        lexer->result_symbol = SPACE;
        return true;
      }
      if (lexer->lookahead == '.') {
        advance(lexer);
        lexer->mark_end(lexer);
        if (valid[FIELD_DOT] && at_field_name(lexer)) return emit(s, lexer, FIELD_DOT);
        return false;
      }
      if (lexer->lookahead == 'e') {
        if (take_else(lexer) && valid[ELSE]) {
          lexer->mark_end(lexer);
          return emit(s, lexer, ELSE);
        }
        return emit_newline(s, lexer);
      }
      if (lexer->lookahead == '/') {
        advance(lexer);
        if (lexer->lookahead == '/') return false;  // a line comment; the line break follows it
        if (lexer->lookahead == '*' && valid[BLOCK_COMMENT]) {
          advance(lexer);
          finish_block_comment(lexer);
          return emit_code_comment(s, lexer);
        }
      }
      return emit_newline(s, lexer);
    }
    if (newline) {
      if (lexer->lookahead == '/') {
        // A block comment after the line break: it is a token of its own, so
        // it takes the whitespace before it along, and the line break is
        // remembered for the scan that comes after it.
        advance(lexer);
        if (lexer->lookahead == '*' && valid[BLOCK_COMMENT]) {
          advance(lexer);
          finish_block_comment(lexer);
          s->pending_newline = true;
          return emit_code_comment(s, lexer);
        }
        if (lexer->lookahead == '/') {
          while (!lexer->eof(lexer) && !is_newline(lexer->lookahead)) advance(lexer);
          if (continues_statement(lexer)) return false;
        }
        return emit_newline(s, lexer);
      }
      if (continues_statement(lexer)) return false;
      return emit_newline(s, lexer);
    }
    // Spaces on the same line. They are a token of their own, so that what
    // follows, a `[` or a backtick perhaps, is scanned from where it starts.
    if (spaced) {
      lexer->mark_end(lexer);
      lexer->result_symbol = SPACE;
      return true;
    }
  }

  bool spaced = false, newline = false;
  while (is_space(lexer->lookahead)) {
    if (is_newline(lexer->lookahead)) newline = true;
    spaced = true;
    skip(lexer);
  }
  int32_t c = lexer->lookahead;
  if (s->comment_then_open && (c == '(' || c == '[') &&
      lexer->get_column(lexer) == s->comment_then_open_col) {
    spaced = true;
  }

  if (c == '(' && !spaced && valid[CALL_PAREN]) {
    advance(lexer);
    lexer->mark_end(lexer);
    return emit(s, lexer, CALL_PAREN);
  }
  if (c == '[') {
    if (!spaced && valid[CALL_BRACKET]) {
      advance(lexer);
      lexer->mark_end(lexer);
      return emit_opening(s, lexer, CALL_BRACKET);
    }
    if (valid[CONTENT_OPEN]) {
      advance(lexer);
      lexer->mark_end(lexer);
      return emit_opening(s, lexer, CONTENT_OPEN);
    }
    return false;
  }
  if (c == '.' && valid[FIELD_DOT] && (!spaced || newline)) {
    advance(lexer);
    lexer->mark_end(lexer);
    if (!at_field_name(lexer)) return false;
    return emit(s, lexer, FIELD_DOT);
  }
  if (c >= '0' && c <= '9' && valid[TRAILING_DOT_FLOAT]) {
    // `1.` is a float when nothing that could continue it follows: not `1..`,
    // not `1.5` (the internal lexer's) and not a method, `1.max(2)`.
    while (lexer->lookahead >= '0' && lexer->lookahead <= '9') advance(lexer);
    if (lexer->lookahead != '.') return false;
    advance(lexer);
    int32_t next = lexer->lookahead;
    if (next == '.' || (next >= '0' && next <= '9') || is_id_start(next)) return false;
    lexer->mark_end(lexer);
    return emit(s, lexer, TRAILING_DOT_FLOAT);
  }
  if (c == '`' && (valid[RAW_OPEN_INLINE] || valid[RAW_OPEN_BLOCK])) {
    return scan_raw_open(s, lexer, valid);
  }
  if (valid[MATH_ARG_NAME] && is_id_start(c)) {
    // `delim: "["` in a math call; not `a := b` or `a ::= b`.
    while (is_id_continue(lexer->lookahead)) advance(lexer);
    lexer->mark_end(lexer);
    if (lexer->lookahead != ':') return false;
    advance(lexer);
    if (lexer->lookahead == '=' || lexer->lookahead == ':') return false;
    return emit(s, lexer, MATH_ARG_NAME);
  }
  if (c == '/' && valid[BLOCK_COMMENT]) {
    advance(lexer);
    if (lexer->lookahead != '*') return false;
    advance(lexer);
    finish_block_comment(lexer);
    return emit_code_comment(s, lexer);
  }
  return false;
}

// ---- markup -----------------------------------------------------------------

// Text runs on until a character that may start something else
// (`Lexer::text` and its table). `prev` is the last character already taken.
static bool scan_text(Scanner *s, TSLexer *lexer, int32_t prev) {
  for (;;) {
    int32_t c = lexer->lookahead;
    if (lexer->eof(lexer)) break;
    if (c == ' ') {
      // `a b` is one text node; a space before anything else is whitespace.
      advance(lexer);
      if (!is_alnum(lexer->lookahead)) break;
      lexer->mark_end(lexer);
      prev = c;
      continue;
    }
    if (is_markup_space(c)) break;
    switch (c) {
      // Smart quotes and colons are tokens of their own in Typst, so what
      // follows them starts afresh: `"-1"` has a minus sign.
      case '\\': case '[': case ']': case '~': case '`': case '$': case '#': case '<':
      case '\'': case '"': case ':':
        goto done;
      case '*': case '_':
        // Inside a word on both sides it is text: `snake_case`, `a*b`.
        advance(lexer);
        if (!is_wordy(prev) || !is_wordy(lexer->lookahead)) goto done;
        lexer->mark_end(lexer);
        prev = c;
        continue;
      case '/':
        advance(lexer);
        if (lexer->lookahead == '/' || lexer->lookahead == '*') goto done;
        lexer->mark_end(lexer);
        prev = c;
        continue;
      case '-':
        advance(lexer);
        if (lexer->lookahead == '-' || lexer->lookahead == '?') goto done;
        lexer->mark_end(lexer);
        prev = c;
        continue;
      case '.':
        advance(lexer);
        if (lexer->lookahead == '.') {
          advance(lexer);
          if (lexer->lookahead == '.') goto done;
          prev = '.';
        } else {
          prev = c;
        }
        lexer->mark_end(lexer);
        continue;
      case '@':
        advance(lexer);
        if (is_id_continue(lexer->lookahead)) goto done;
        lexer->mark_end(lexer);
        prev = c;
        continue;
      case 'h': {
        // `http://` and `https://` start a link even mid-text.
        advance(lexer);
        const char *rest = "ttp";
        bool link = true;
        for (int i = 0; rest[i]; i++) {
          if (lexer->lookahead != rest[i]) { link = false; break; }
          advance(lexer);
        }
        if (link) {
          if (lexer->lookahead == 's') advance(lexer);
          const char *sep = "://";
          for (int i = 0; sep[i]; i++) {
            if (lexer->lookahead != sep[i]) { link = false; break; }
            advance(lexer);
          }
          if (link) goto done;
        }
        lexer->mark_end(lexer);
        prev = 'p';
        continue;
      }
      default:
        break;
    }
    advance(lexer);
    lexer->mark_end(lexer);
    prev = c;
  }
done:
  return emit(s, lexer, TEXT);
}

// Typst's `space_or_end`: a marker needs whitespace, a comment or the end after
// it. Warning: **a `/` is consumed to see what follows it**, so the caller has
// called `mark_end` before asking.
static inline bool at_space_or_end(TSLexer *lexer) {
  if (lexer->eof(lexer) || is_markup_space(lexer->lookahead)) return true;
  if (lexer->lookahead != '/') return false;
  advance(lexer);
  return lexer->lookahead == '/' || lexer->lookahead == '*';
}

// At `\`: an escape, or a line break when whitespace or the end follows.
static bool scan_backslash(Scanner *s, TSLexer *lexer, const bool *valid) {
  advance(lexer);
  if (lexer->eof(lexer) || is_space(lexer->lookahead)) {
    lexer->mark_end(lexer);
    return valid[LINEBREAK] && emit(s, lexer, LINEBREAK);
  }
  if (lexer->lookahead == 'u') {
    advance(lexer);
    lexer->mark_end(lexer);
    if (lexer->lookahead == '{') {
      advance(lexer);
      while (is_alnum(lexer->lookahead) && lexer->lookahead < 0x80) advance(lexer);
      if (lexer->lookahead == '}') advance(lexer);
      lexer->mark_end(lexer);
    }
  } else {
    advance(lexer);
    lexer->mark_end(lexer);
  }
  return valid[ESCAPE] && emit(s, lexer, ESCAPE);
}

// After `http://` or `https://`: brackets must balance, and trailing
// punctuation belongs to the sentence (`link_prefix`).
static bool finish_url(Scanner *s, TSLexer *lexer) {
  lexer->mark_end(lexer);
  char stack[64];
  int depth = 0;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (c == '(' || c == '[') {
      if (depth < 64) stack[depth] = (char)c;
      depth++;
    } else if (c == ')' || c == ']') {
      if (depth == 0) break;
      depth--;
      if (depth < 64 && stack[depth] != (c == ')' ? '(' : '[')) break;
    } else if (!is_url_char(c)) {
      break;
    }
    advance(lexer);
    if (!is_url_trailing(c)) lexer->mark_end(lexer);
  }
  return emit(s, lexer, URL);
}

static bool scan_markup(Scanner *s, TSLexer *lexer, const bool *valid) {
  int32_t c = lexer->lookahead;

  // ---- whitespace: close what the line break closes, or be a space token.
  if (is_markup_space(c)) {
    lexer->mark_end(lexer);
    uint32_t newlines = 0;
    while (is_markup_space(lexer->lookahead)) {
      int32_t w = lexer->lookahead;
      advance(lexer);
      if (is_newline(w)) {
        if (w == '\r' && lexer->lookahead == '\n') advance(lexer);
        newlines++;
      }
    }
    // A `]` that closes a text bracket goes first; one that cannot ends the
    // structures inside the block it closes.
    bool end = lexer->eof(lexer) || (lexer->lookahead == ']' && !valid[RBRACKET]);
    // Warning: **a comment's column decides like any other token's.** Typst
    // looks past comments to the first real token, and waiting for it would
    // put the closing after the comment, inside the item; a comment line less
    // indented than the item's text it sits in is rarer than a term item
    // straight after a list item, which waiting got wrong.
    // A strong or emphasis still open at a paragraph break.
    if (newlines >= 2 && valid[UNCLOSED]) return emit_zero(lexer, UNCLOSED);
    if (newlines > 0 || end) {
      if (close_structure(s, lexer, valid, newlines > 0 || s->line_start, end, -1)) return true;
    } else if (lexer->lookahead == '/' && !s->block_start && !s->line_start &&
               (valid[HEADING_END] || valid[ITEM_END])) {
      // A comment at the end of a heading's or an item's line. Typst ends the
      // structure before it (trailing trivia stay outside), so the line break
      // after the comment decides, here, before the spaces.
      advance(lexer);
      if (lexer->lookahead == '/') {
        while (!lexer->eof(lexer) && !is_newline(lexer->lookahead)) advance(lexer);
        while (is_markup_space(lexer->lookahead)) advance(lexer);
        bool at_end = lexer->eof(lexer) || (lexer->lookahead == ']' && !valid[RBRACKET]);
        if (close_structure(s, lexer, valid, true, at_end, -1)) return true;
        // Not closed: the spaces and the comment are the internal lexer's.
        return false;
      }
      if (lexer->lookahead == '*' && valid[BLOCK_COMMENT]) {
        // The comment takes the spaces before it along.
        advance(lexer);
        finish_block_comment(lexer);
        lexer->result_symbol = BLOCK_COMMENT;
        s->after_space = true;
        return true;
      }
      // A slash in text, which takes the spaces before it along.
      lexer->mark_end(lexer);
      return valid[TEXT] && scan_text(s, lexer, '/');
    } else if (lexer->lookahead == '<' && valid[HEADING_END]) {
      // A heading stops before its label (`heading` in `parser.rs`). In
      // markup a `<` and a letter always open a label, or Typst rejects the
      // file, so the letter need not be seen.
      return emit_zero(lexer, HEADING_END);
    }
    lexer->mark_end(lexer);
    lexer->result_symbol = newlines >= 2 ? PARBREAK : SPACE;
    s->after_space = true;
    if (newlines > 0) s->line_start = true;
    return true;
  }

  lexer->mark_end(lexer);
  if (lexer->eof(lexer)) return close_structure(s, lexer, valid, s->line_start, true, -1);

  bool at_start = s->line_start || s->block_start;
  // The column of a marker, which is only asked for when one may start here.
  uint32_t col = at_start ? lexer->get_column(lexer) : 0;

  if (c == '<' && valid[HEADING_END]) {
    advance(lexer);
    if (is_id_continue(lexer->lookahead)) return emit_zero(lexer, HEADING_END);
    lexer->mark_end(lexer);
    return valid[TEXT] && scan_text(s, lexer, '<');
  }

  // ---- comments, and closings at the start of a line.
  if (c == '/') {
    advance(lexer);
    if (lexer->lookahead == '/') return false;  // a line comment: the internal lexer's
    if (lexer->lookahead == '*') {
      if (!valid[BLOCK_COMMENT]) return false;
      advance(lexer);
      finish_block_comment(lexer);
      lexer->result_symbol = BLOCK_COMMENT;
      s->after_space = true;
      return true;
    }
    // The column was taken before the slash was consumed.
    if (s->line_start && close_structure(s, lexer, valid, true, false, col)) return true;
    lexer->mark_end(lexer);
    if (at_start && valid[TERM_MARKER] && at_space_or_end(lexer)) {
      return emit_item_marker(s, lexer, TERM_MARKER, col);
    }
    return valid[TEXT] && scan_text(s, lexer, '/');
  }
  bool closing = c == ']' && !valid[RBRACKET];
  if (s->line_start && close_structure(s, lexer, valid, true, closing, col)) return true;
  if (c == ']') {
    if (closing) return close_structure(s, lexer, valid, false, true, -1);
    advance(lexer);
    lexer->mark_end(lexer);
    return emit(s, lexer, RBRACKET);
  }

  bool direct = !s->after_space;

  switch (c) {
    case '(':
      if (direct && valid[CALL_PAREN]) {
        advance(lexer);
        lexer->mark_end(lexer);
        return emit(s, lexer, CALL_PAREN);
      }
      break;

    case '[':
      advance(lexer);
      lexer->mark_end(lexer);
      if (direct && valid[CALL_BRACKET]) return emit_opening(s, lexer, CALL_BRACKET);
      if (direct && valid[CONTENT_OPEN]) return emit_opening(s, lexer, CONTENT_OPEN);
      if (valid[TEXT_LBRACKET]) return emit(s, lexer, TEXT_LBRACKET);
      return valid[TEXT] && emit(s, lexer, TEXT);

    case '.':
      advance(lexer);
      lexer->mark_end(lexer);
      if (direct && valid[FIELD_DOT] && lexer->lookahead != '_' && is_id_start(lexer->lookahead)) {
        return emit(s, lexer, FIELD_DOT);
      }
      if (direct && valid[FIELD_DOT] && lexer->lookahead == '_') {
        advance(lexer);
        if (is_id_continue(lexer->lookahead)) return emit(s, lexer, FIELD_DOT);
        // The `_` may close emphasis: the text is the dot alone.
        return valid[TEXT] && emit(s, lexer, TEXT);
      }
      if (lexer->lookahead == '.') {
        advance(lexer);
        if (lexer->lookahead == '.' && valid[SHORTHAND]) {
          advance(lexer);
          lexer->mark_end(lexer);
          return emit(s, lexer, SHORTHAND);
        }
        lexer->mark_end(lexer);
      }
      return valid[TEXT] && scan_text(s, lexer, '.');

    case 'e':
      if (valid[ELSE] && !s->line_start) {
        if (take_else(lexer)) {
          lexer->mark_end(lexer);
          return emit(s, lexer, ELSE);
        }
        lexer->mark_end(lexer);
        return valid[TEXT] && scan_text(s, lexer, 'e');
      }
      break;

    case '`':
      if (valid[RAW_OPEN_INLINE] || valid[RAW_OPEN_BLOCK]) return scan_raw_open(s, lexer, valid);
      break;

    case '\\':
      return scan_backslash(s, lexer, valid);

    case '#':
      if (valid[HASH]) {
        advance(lexer);
        if (lexer->lookahead == '!') return false;  // a shebang: the internal lexer's
        lexer->mark_end(lexer);
        return emit(s, lexer, HASH);
      }
      break;

    case '$':
      if (valid[DOLLAR]) {
        advance(lexer);
        lexer->mark_end(lexer);
        return emit(s, lexer, DOLLAR);
      }
      break;

    case '*':
      if (valid[STAR]) {
        advance(lexer);
        lexer->mark_end(lexer);
        return emit(s, lexer, STAR);
      }
      break;

    case '_':
      if (valid[UNDERSCORE]) {
        advance(lexer);
        lexer->mark_end(lexer);
        return emit(s, lexer, UNDERSCORE);
      }
      break;

    case '<': {
      advance(lexer);
      lexer->mark_end(lexer);
      if (is_id_continue(lexer->lookahead)) {
        while (is_label_char(lexer->lookahead)) advance(lexer);
        if (lexer->lookahead == '>' && valid[LABEL]) {
          advance(lexer);
          lexer->mark_end(lexer);
          return emit(s, lexer, LABEL);
        }
      }
      return valid[TEXT] && scan_text(s, lexer, '<');
    }

    case '@': {
      advance(lexer);
      lexer->mark_end(lexer);
      if (is_id_continue(lexer->lookahead) && valid[REF_MARKER]) {
        // Trailing `.` and `:` belong to the sentence.
        while (is_label_char(lexer->lookahead)) {
          int32_t l = lexer->lookahead;
          advance(lexer);
          if (l != '.' && l != ':') lexer->mark_end(lexer);
        }
        return emit(s, lexer, REF_MARKER);
      }
      return valid[TEXT] && scan_text(s, lexer, '@');
    }

    case 'h': {
      advance(lexer);
      lexer->mark_end(lexer);
      const char *rest = "ttp";
      for (int i = 0; rest[i]; i++) {
        if (lexer->lookahead != rest[i]) goto plain_h;
        advance(lexer);
      }
      if (lexer->lookahead == 's') advance(lexer);
      for (int i = 0; i < 3; i++) {
        if (lexer->lookahead != "://"[i]) goto plain_h;
        advance(lexer);
      }
      if (valid[URL]) return finish_url(s, lexer);
    plain_h:
      lexer->mark_end(lexer);
      return valid[TEXT] && scan_text(s, lexer, 'h');
    }

    case '~':
      if (valid[SHORTHAND]) {
        advance(lexer);
        lexer->mark_end(lexer);
        return emit(s, lexer, SHORTHAND);
      }
      break;

    case '-': {
      advance(lexer);
      lexer->mark_end(lexer);
      if (at_start && valid[LIST_MARKER] && lexer->lookahead != '-' && lexer->lookahead != '?' &&
          !is_numeric(lexer->lookahead) && at_space_or_end(lexer)) {
        return emit_item_marker(s, lexer, LIST_MARKER, col);
      }
      if (valid[SHORTHAND]) {
        if (lexer->lookahead == '-') {
          advance(lexer);
          if (lexer->lookahead == '-') advance(lexer);
          lexer->mark_end(lexer);
          return emit(s, lexer, SHORTHAND);
        }
        if (lexer->lookahead == '?') {
          advance(lexer);
          lexer->mark_end(lexer);
          return emit(s, lexer, SHORTHAND);
        }
        // A hyphen before a digit is a minus sign: `-1`.
        if (is_numeric(lexer->lookahead)) return emit(s, lexer, SHORTHAND);
      }
      return valid[TEXT] && scan_text(s, lexer, '-');
    }

    case '+':
      advance(lexer);
      lexer->mark_end(lexer);
      if (at_start && valid[ENUM_MARKER] && at_space_or_end(lexer)) {
        return emit_item_marker(s, lexer, ENUM_MARKER, col);
      }
      return valid[TEXT] && scan_text(s, lexer, '+');

    case '=':
      while (lexer->lookahead == '=') advance(lexer);
      lexer->mark_end(lexer);
      if (at_start && valid[HEADING_MARKER] && at_space_or_end(lexer)) {
        return emit(s, lexer, HEADING_MARKER);
      }
      return valid[TEXT] && scan_text(s, lexer, '=');

    case ':':
    case '\'':
    case '"':
      advance(lexer);
      lexer->mark_end(lexer);
      if (c == ':' && valid[TERM_COLON]) return emit_opening(s, lexer, TERM_COLON);
      return valid[TEXT] && emit(s, lexer, TEXT);

    default:
      if (c >= '0' && c <= '9') {
        // `1.` before whitespace numbers an item.
        int32_t last = c;
        while (lexer->lookahead >= '0' && lexer->lookahead <= '9') advance(lexer);
        lexer->mark_end(lexer);
        if (lexer->lookahead == '.') {
          advance(lexer);
          if (lexer->lookahead == '.') {
            // `1..` is text up to the dots, which may be a shorthand.
            return valid[TEXT] && emit(s, lexer, TEXT);
          }
          lexer->mark_end(lexer);
          if (at_start && valid[ENUM_MARKER] && at_space_or_end(lexer)) {
            return emit_item_marker(s, lexer, ENUM_MARKER, col);
          }
          last = '.';
        }
        return valid[TEXT] && scan_text(s, lexer, last);
      }
      break;
  }

  if (!valid[TEXT]) return false;
  advance(lexer);
  lexer->mark_end(lexer);
  return scan_text(s, lexer, c);
}

bool tree_sitter_typst_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid) {
  Scanner *s = payload;

  if (valid[ERROR_SENTINEL]) {
    // Recovering: everything is valid, so nothing here can be trusted to mean
    // anything. Keep raw text and comments whole, offer runs of words as text
    // so that markup after a broken expression is found again sooner (of the
    // files Typst rejects, 17.2 percent of the bytes end up inside ERROR nodes
    // rather than 18.1), and let the rest fall through.
    if (s->raw_ticks > 0) return scan_raw_inside(s, lexer, valid);
    while (is_space(lexer->lookahead)) skip(lexer);
    if (lexer->lookahead == '/') {
      advance(lexer);
      if (lexer->lookahead != '*') return false;
      advance(lexer);
      finish_block_comment(lexer);
      lexer->result_symbol = BLOCK_COMMENT;
      return true;
    }
    if (is_alnum(lexer->lookahead) && !(lexer->lookahead >= '0' && lexer->lookahead <= '9')) {
      while (is_alnum(lexer->lookahead) || lexer->lookahead == ' ') advance(lexer);
      lexer->mark_end(lexer);
      lexer->result_symbol = TEXT;
      return true;
    }
    return false;
  }

  if (s->raw_ticks > 0 && (valid[RAW_LANG] || valid[RAW_TEXT] || valid[RAW_CLOSE])) {
    return scan_raw_inside(s, lexer, valid);
  }

  // Inside a string nothing is a comment: `"//"` and `"/*"` are strings.
  if (valid[STRING_CONTENT]) {
    bool any = false;
    while (!lexer->eof(lexer) && lexer->lookahead != '"' && lexer->lookahead != '\\') {
      advance(lexer);
      any = true;
    }
    if (any) {
      lexer->mark_end(lexer);
      lexer->result_symbol = STRING_CONTENT;
      return true;
    }
    return false;
  }

  if (valid[TEXT]) return scan_markup(s, lexer, valid);
  return scan_code(s, lexer, valid);
}
