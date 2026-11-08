/// <reference types="tree-sitter-cli/dsl" />
// @ts-check

// **Typst, written from scratch.**
//
// Every rule here is read off Typst's own parser, `crates/typst-syntax` in
// typst/typst (`lexer.rs`, `parser.rs`, `set.rs`, `kind.rs`), and checked
// against real `.typ` files: the compiler's test suite, its documentation and
// the latest version of every package in typst/packages (`script/baseline.sh`).
//
// Typst has three modes and the lexer changes with them: the same `-` is a list
// marker, a shorthand or text in markup, an operator in code and a shorthand in
// math. tree-sitter lexes per parse state, so the mode is simply the state.
// What a state machine cannot see is everything Typst decides by looking around
// a token, and that is the external scanner's job (`src/scanner.c`):
//
// - markup is lexed there entirely: whether `*` opens strong depends on the
//   characters on both sides of it, whether `-` is a list marker depends on
//   being at the start of a line, and a list item lasts as long as the lines
//   below it are indented deeper than its marker;
// - whitespace decides structure in code: `#f(x)` is a call and `#f (x)` is a
//   name followed by text, `#x.y` is a field access and `#x. Y` is a sentence,
//   and a statement in a code block ends at a line break unless the next line
//   goes on with `.` or `else`;
// - raw text is closed by as many backticks as opened it, and block comments
//   nest.
//
// Warning: **the scanner keys off which external tokens are valid.** It tells
// markup from code by asking whether `text` is valid, and tells "right after an
// expression" from "anywhere else" by asking whether `_call_paren` is. tree-sitter
// never merges two parse states when that would make an external token valid in
// a state that did not have it (`token_conflicts` in `minimize_parse_table.rs`),
// so those answers are exact. Do not give a markup rule a code follower, or the
// other way round, without reading the scanner.

// Binding strength in code, loosest first, as `ast.rs` gives it
// (`BinOp::precedence`, `UnOp::precedence`).
const PREC = {
  statement: -1, // let, set, show, import, include, return, context, closures
  assign: 1,
  or: 2,
  and: 3,
  compare: 4, // also `in`, `not in` and unary `not`
  add: 5,
  mul: 6,
  unary: 7, // `+x`, `-x`
  postfix: 8, // calls and field access
};

// Math binding strength (`parser.rs`, `math_op`): fractions are looser than
// attachments, and an implicit call is as tight as an attachment.
const MATH_PREC = {
  frac: 1,
  attach: 2,
};

// Characters Typst's default math class calls opening or closing
// (`typst_utils::default_math_class` over `unicode-math-class`), less the
// parentheses, which are tokens of their own because `f(x)` is a call.
const MATH_OPEN = '[{⌈⌊⌜⌞❲⟦⟨⟪⟬⟮⦃⦅⦇⦉⦋⦍⦏⦑⦓⦕⦗⧘⧚⧼⎰⟅';
const MATH_CLOSE = ']}⌉⌋⌝⌟❳⟧⟩⟫⟭⟯⦄⦆⦈⦊⦌⦎⦐⦒⦔⦖⦘⧙⧛⧽⎱⟆';

// Math shorthands, longest first (`lexer.rs`, `Lexer::math`). `[|` and `|]`
// are delimiters and live with the other openings and closings.
const MATH_SHORTHANDS = [
  '<==>', '<-->', '->>', '-->', '::=', '<--', '<-<', '<->', '<<-', '<<<', '<=>',
  '<==', '<~~', '>->', '>>>', '==>', '|->', '|=>', '~~>', '->', ':=', '!=', '...',
  '<=', '<<', '<-', '<~', '=>', '=:', '>=', '>>', '||', '~>', '*', '-', '~',
];

// Units a number may carry (`lexer.rs`, `Lexer::number`).
const UNITS = ['pt', 'mm', 'cm', 'in', 'deg', 'rad', 'em', 'fr', '%'];

// Unicode whitespace, which Typst skips in code and math (`char::is_whitespace`).
// Warning: **`\s` alone misses the no-break space**, and real files have it.
const SPACE = '\\s\\u00A0\\u1680\\u2000-\\u200A\\u2028\\u2029\\u202F\\u205F\\u3000\\uFEFF';

/** @param {RuleOrLiteral} rule */
const commaSep = rule => seq(rule, repeat(seq(',', rule)), optional(','));

module.exports = grammar({
  name: 'typst',

  // Warning: **the order is the scanner's `enum TokenType`.** Add at the end,
  // or renumber both.
  externals: $ => [
    $._space, // markup whitespace without a blank line in it
    $.parbreak, // markup whitespace with a blank line in it
    $.block_comment, // `/* … */`, nested
    $._newline, // ends a statement in a code block, unless `.` or `else` goes on
    $._item_end, // ends a list, enum or term item (zero width)
    $._heading_end, // ends a heading (zero width)
    $.text,
    $._hash, // `#` in markup
    $._dollar, // `$` opening an equation in markup
    $._content_open, // `[` opening a content block
    $._call_bracket, // `[` straight after a callee: a trailing content argument
    $._text_lbracket, // `[` in markup, which is text
    $._rbracket, // `]` in markup: closes a content block or a text bracket
    $._call_paren, // `(` straight after a callee
    $._field_dot, // `.` straight before a field name
    $._else, // `else` after a block, where markup would otherwise take it
    $._star, // `*` delimiting strong emphasis
    $._underscore, // `_` delimiting emphasis
    $.heading_marker,
    $.list_marker,
    $.enum_marker,
    $.term_marker,
    $._term_colon,
    $.escape,
    $.linebreak,
    $.shorthand,
    $.url,
    $.label,
    $.ref_marker,
    $._raw_open_inline,
    $._raw_open_block,
    $.raw_lang,
    $.raw_text,
    $._raw_close,
    $._math_arg_name,
    $._trailing_dot_float, // `1.`: a float only when no method name follows
    $._string_content, // so that `"//"` is a string and not a comment
    $._line_end, // ends a statement after a hash: a line break, `]` or the end
    $._unclosed, // where Typst gives up on an unclosed `*` or `_`
    $._math_spread_dots, // `..` opening a math argument, when something follows
    $._error_sentinel, // never valid in a real state: tells the scanner it is recovering
  ],

  extras: $ => [new RegExp(`[${SPACE}]`), $.line_comment, $.block_comment, $.shebang, $._space, $.parbreak],

  word: $ => $.identifier,

  supertypes: $ => [$._expr],

  rules: {
    source_file: $ => repeat($._markup),

    // ---- markup -------------------------------------------------------------

    _markup: $ => choice($._inline, $.heading, $.list_item, $.enum_item, $.term_item),

    _inline: $ => choice($._inline_common, $.strong, $.emph),

    // Strong cannot hold strong (the next `*` closes it) and emphasis cannot
    // hold emphasis, so each body leaves its own delimiter out.
    _inline_common: $ => choice(
      $.text,
      $.escape,
      $.linebreak,
      $.shorthand,
      $.url,
      $.label,
      $.reference,
      $._raw,
      $.equation,
      $._embedded,
      $._text_bracket,
    ),

    // Typst gives up on a strong or emphasis that is still open at a paragraph
    // break, at the `]` of its block or at the end (`AtNewline::StopParBreak`)
    // and reports it. Here it simply ends there, so that typing a `*` does not
    // turn the rest of the document bold until its partner is typed. Files
    // Typst accepts never get this far.
    strong: $ => seq(
      alias($._star, '*'),
      repeat(choice($._inline_common, $.emph)),
      choice(alias($._star, '*'), $._unclosed),
    ),

    emph: $ => seq(
      alias($._underscore, '_'),
      repeat(choice($._inline_common, $.strong)),
      choice(alias($._underscore, '_'), $._unclosed),
    ),

    // A `[` in markup is text, and so is the `]` that matches it: Typst counts
    // them so that the inner `]` of `#[a [b] c]` does not close the block.
    // Typst lets an unmatched `[` run to the end of the file, hence the
    // optional close; `prec.right` makes a `]` that can close the bracket do so.
    _text_bracket: $ => prec.right(seq(
      alias($._text_lbracket, $.text),
      repeat($._markup),
      optional(alias($._rbracket, $.text)),
    )),

    // A heading ends at the end of its line (`AtNewline::Stop`) or before a
    // label, which belongs to the heading without being part of it.
    heading: $ => seq($.heading_marker, repeat($._inline), $._heading_end),

    // An item lasts while the lines below it are indented deeper than its
    // marker (`AtNewline::RequireColumn`); the scanner keeps the columns.
    list_item: $ => seq($.list_marker, repeat($._markup), $._item_end),

    enum_item: $ => seq($.enum_marker, repeat($._markup), $._item_end),

    term_item: $ => seq(
      $.term_marker,
      optional(field('term', $.term)),
      alias($._term_colon, ':'),
      repeat($._markup),
      $._item_end,
    ),

    term: $ => repeat1($._inline),

    reference: $ => seq(
      $.ref_marker,
      optional(field('supplement', $.content_block)),
    ),

    // `#` and one expression. After the hash Typst parses an *atomic*
    // expression: `#x + 1` is `x` followed by the text ` + 1`, and only calls
    // and field accesses written without a space go on. Statements are the
    // exception: they run to the end of the line or a `;`, and unlike in a code
    // block a next line starting with `.` does not continue them
    // (`AtNewline::Stop`), hence `_line_end` rather than `_newline`.
    _embedded: $ => choice(
      alias($._embedded_expression, $.code),
      seq(alias($._embedded_statement, $.code), choice(';', $._line_end)),
    ),

    _embedded_expression: $ => seq(
      alias($._hash, '#'),
      choice($._postfix, alias($._atomic_contextual, $.contextual)),
    ),

    _embedded_statement: $ => seq(alias($._hash, '#'), $._statement),

    // ---- raw ----------------------------------------------------------------

    _raw: $ => choice($.raw_span, $.raw_block),

    raw_span: $ => seq($._raw_open_inline, optional($.raw_text), $._raw_close),

    raw_block: $ => seq(
      $._raw_open_block,
      optional(field('language', $.raw_lang)),
      optional($.raw_text),
      $._raw_close,
    ),

    // ---- code ---------------------------------------------------------------

    _expr: $ => choice(
      $._postfix,
      $.unary,
      $.binary,
      $.destructuring_assignment,
      $.closure,
      $._statement,
      $.contextual,
    ),

    // Everything a hash may introduce, and what calls and field accesses
    // attach to.
    _postfix: $ => choice($._atomic, $.call, $.field_access),

    _atomic: $ => choice(
      $.identifier,
      '_',
      $.none,
      $.auto,
      $.bool,
      $.int,
      $.float,
      alias($._trailing_dot_float, $.float),
      $.numeric,
      $.string,
      alias($._code_label, $.label),
      $._raw,
      $.code_block,
      $.content_block,
      $.parenthesized,
      $.array,
      $.dict,
      $.equation,
      $.conditional,
      $.for_loop,
      $.while_loop,
      $.break_statement,
      $.continue_statement,
    ),

    _statement: $ => choice(
      $.let_binding,
      $.set_rule,
      $.show_rule,
      $.module_import,
      $.module_include,
      $.return_statement,
    ),

    // Any expression followed *directly* by `(` or `[` is called: Typst checks
    // `directly_at`, so the scanner hands out `_call_paren` and `_call_bracket`
    // only when nothing, not even a comment, comes in between.
    call: $ => prec(PREC.postfix, seq(
      field('function', $._postfix),
      field('arguments', $.arguments),
    )),

    // `f[a][b]` is one call with two trailing blocks, not a call of a call, as
    // `args` in `parser.rs` reads it; the blocks are a right-recursive run for
    // the reason given at `math_attach`.
    arguments: $ => prec.right(choice(
      seq(alias($._call_paren, '('), optional(commaSep($._item)), ')', optional($._trailing_blocks)),
      $._trailing_blocks,
    )),

    _trailing_blocks: $ => prec.right(seq(
      alias($._trailing_block, $.content_block),
      optional($._trailing_blocks),
    )),

    _trailing_block: $ => seq(
      alias($._call_bracket, '['),
      repeat($._markup),
      alias($._rbracket, ']'),
    ),

    // The dot after a callee comes from the scanner when it must be written
    // without a space (after a hash); in code a plain `.` is allowed too, so a
    // method chain may go on on the next line.
    field_access: $ => prec(PREC.postfix, seq(
      field('object', $._postfix),
      choice(alias($._field_dot, '.'), '.'),
      field('field', $.identifier),
    )),

    unary: $ => choice(
      prec(PREC.unary, seq(field('operator', choice('+', '-', '−')), field('operand', $._expr))),
      prec.right(PREC.compare, seq(field('operator', 'not'), field('operand', $._expr))),
    ),

    binary: $ => {
      /** @type {[RuleOrLiteral, number][]} */
      const left = [
        ['*', PREC.mul], ['/', PREC.mul],
        ['+', PREC.add], ['-', PREC.add], ['−', PREC.add],
        ['==', PREC.compare], ['!=', PREC.compare],
        ['<', PREC.compare], ['<=', PREC.compare],
        ['>', PREC.compare], ['>=', PREC.compare],
        ['in', PREC.compare], [seq('not', 'in'), PREC.compare],
        ['and', PREC.and], ['or', PREC.or],
      ];
      const right = ['=', '+=', '-=', '−=', '*=', '/='];
      return choice(
        ...left.map(([op, p]) => prec.left(p, seq(
          field('left', $._expr),
          field('operator', op),
          field('right', $._expr),
        ))),
        ...right.map(op => prec.right(PREC.assign, seq(
          field('left', $._expr),
          field('operator', op),
          field('right', $._expr),
        ))),
      );
    },

    // `(a, b) = (b, a)`, `(x: a) = d`, `_ = f()`. A parenthesized `(x) = 1` is
    // an ordinary assignment, as in Typst.
    destructuring_assignment: $ => prec.right(PREC.assign + 1, seq(
      field('pattern', choice($.array, $.dict, '_')),
      '=',
      field('value', $._expr),
    )),

    // `x => …`, `_ => …`, `(a, b: 1, ..rest) => …`. The parameter list is read
    // as the parenthesized expression, array or dictionary it looks like
    // (Typst backtracks to the same effect), so a default `b: 1` is a `named`.
    closure: $ => prec.right(PREC.statement, seq(
      field('parameters', choice($.identifier, '_', $.parenthesized, $.array, $.dict)),
      '=>',
      field('body', $._expr),
    )),

    contextual: $ => prec.right(PREC.statement, seq('context', field('body', $._expr))),

    // After a hash, `context` takes an atomic expression like the hash does.
    _atomic_contextual: $ => seq('context', field('body', $._postfix)),

    code_block: $ => seq('{', optional($._code), '}'),

    // Statements end at a line break or a semicolon (`code_exprs`).
    _code: $ => seq(
      repeat(';'),
      $._expr,
      repeat(seq($._separator, $._expr)),
      optional($._separator),
    ),

    // Warning: **`_newline` may not follow `_newline`.** It is zero-width and
    // sits before the line break, so a grammar that accepted two in a row would
    // be handed the same one forever.
    _separator: $ => choice(seq($._newline, repeat(';')), repeat1(';')),

    content_block: $ => seq(
      alias($._content_open, '['),
      repeat($._markup),
      alias($._rbracket, ']'),
    ),

    parenthesized: $ => seq('(', field('expression', $._expr), ')'),

    // `(a,)`, `(a, b)`, `()`, `(..a)`. Typst decides between an array and a
    // dictionary by the first pair it meets; here the first item decides, and
    // the items after it may be anything, as a closure's parameters can be.
    array: $ => seq('(', optional(choice(
      seq($._expr, ',', optional(commaSep($._item))),
      seq($.spread, optional(seq(',', optional(commaSep($._item))))),
    )), ')'),

    // `(:)`, `(a: 1)`, `("key": 2)`, `(: ..d)`.
    dict: $ => seq('(', choice(
      seq(':', optional(commaSep($._item))),
      seq($.named, optional(seq(',', optional(commaSep($._item))))),
    ), ')'),

    _item: $ => choice($._expr, $.named, $.spread),

    named: $ => seq(field('name', $._expr), ':', field('value', $._expr)),

    // `..` alone is an argument sink in a parameter list.
    spread: $ => prec.right(seq('..', optional(field('value', $._expr)))),

    let_binding: $ => prec.right(PREC.statement, seq('let', choice(
      seq(
        field('name', $.identifier),
        field('parameters', $.parameters),
        '=',
        field('value', $._expr),
      ),
      seq(field('pattern', $._pattern), optional(seq('=', field('value', $._expr)))),
    ))),

    parameters: $ => seq(alias($._call_paren, '('), optional(commaSep($._item)), ')'),

    _pattern: $ => choice($.identifier, '_', $.parenthesized, $.array, $.dict),

    // `set text(red)`, `set page.foo(…)`, `set text(red) if cond`.
    set_rule: $ => prec.right(PREC.statement, seq(
      'set',
      field('target', $._set_target),
      field('arguments', $.arguments),
      optional(seq('if', field('condition', $._expr))),
    )),

    _set_target: $ => choice($.identifier, alias($._set_field, $.field_access)),

    _set_field: $ => seq(
      field('object', $._set_target),
      choice(alias($._field_dot, '.'), '.'),
      field('field', $.identifier),
    ),

    // `show heading: it => …`, `show: template`.
    show_rule: $ => prec.right(PREC.statement, seq(
      'show',
      optional(field('selector', $._expr)),
      ':',
      field('transform', $._expr),
    )),

    module_import: $ => prec.right(PREC.statement, seq(
      'import',
      field('source', $._expr),
      optional(seq('as', field('alias', $.identifier))),
      optional(seq(':', choice(
        '*',
        seq('(', optional(commaSep($._import_item)), ')'),
        optional(commaSep($._import_item)),
      ))),
    )),

    _import_item: $ => choice($._import_path, $.renamed_import_item),

    // The `as` after an item renames it, never the module of an import that
    // happens to be written as the source of another import.
    renamed_import_item: $ => prec(1, seq(
      field('path', $._import_path),
      'as',
      field('alias', $.identifier),
    )),

    _import_path: $ => choice($.identifier, $.import_path),

    import_path: $ => seq($.identifier, repeat1(seq('.', $.identifier))),

    module_include: $ => prec.right(PREC.statement, seq('include', field('source', $._expr))),

    return_statement: $ => prec.right(PREC.statement, seq(
      'return',
      optional(field('value', $._expr)),
    )),

    break_statement: _ => 'break',

    continue_statement: _ => 'continue',

    conditional: $ => prec.right(seq(
      'if',
      field('condition', $._expr),
      field('consequence', $._block),
      optional(seq(
        choice(alias($._else, 'else'), 'else'),
        field('alternative', choice($.conditional, $._block)),
      )),
    )),

    for_loop: $ => seq(
      'for',
      field('pattern', $._pattern),
      'in',
      field('iterable', $._expr),
      field('body', $._block),
    ),

    while_loop: $ => seq(
      'while',
      field('condition', $._expr),
      field('body', $._block),
    ),

    _block: $ => choice($.code_block, $.content_block),

    // ---- code tokens --------------------------------------------------------

    // Identifiers are Unicode identifiers that may also contain `-`: `my-fn`
    // is one name, and `a - b` needs its spaces.
    identifier: _ => /[\p{XID_Start}_][\p{XID_Continue}_-]*/,

    none: _ => 'none',
    auto: _ => 'auto',
    bool: _ => choice('true', 'false'),

    int: _ => token(choice(/0x[0-9a-fA-F]+/, /0o[0-7]+/, /0b[01]+/, /[0-9]+/)),

    float: _ => token(choice(
      /[0-9]+\.[0-9]+([eE][+-]?[0-9]+)?/,
      /\.[0-9]+([eE][+-]?[0-9]+)?/,
      /[0-9]+[eE][+-]?[0-9]+/,
    )),

    // A number with a unit: a length, angle, fraction or ratio.
    numeric: _ => token(seq(
      choice(
        /[0-9]+(\.[0-9]+)?([eE][+-]?[0-9]+)?/,
        /\.[0-9]+([eE][+-]?[0-9]+)?/,
      ),
      choice(...UNITS),
    )),

    string: $ => seq(
      '"',
      repeat(choice($._string_content, alias($._string_escape, $.escape))),
      token.immediate('"'),
    ),

    _string_escape: _ => token.immediate(/\\(u\{[0-9a-zA-Z]*\}|[^u]|u)/),

    _code_label: _ => /<[\p{XID_Continue}_\-.:]+>/,

    line_comment: _ => token(seq('//', /[^\r\n\u2028\u2029]*/)),

    // `#!/usr/bin/env typst …` on the first line. Typst only knows it there;
    // anywhere else a `#!` is an error in Typst, so accepting it costs nothing.
    shebang: _ => token(seq('#!', /[^\r\n]*/)),

    // ---- math ---------------------------------------------------------------

    equation: $ => seq(
      choice(alias($._dollar, '$'), '$'),
      optional($.math),
      '$',
    ),

    math: $ => repeat1($._math_item),

    // At the top of an equation an unmatched closing delimiter stands alone
    // (`$[0, 1)$` aside, which is one delimited group).
    _math_item: $ => choice(
      $._math_common,
      ',',
      ';',
      alias(')', $.math_symbol),
      alias($._math_close_char, $.math_symbol),
      alias('|]', $.math_shorthand),
    ),


    // Inside a delimited group the first closing delimiter closes it.
    _math_inner_item: $ => choice($._math_common, ',', ';'),

    // Inside math arguments `,` and `;` separate and `)` ends the list.
    _math_arg_item: $ => choice(
      $._math_common,
      alias($._math_close_char, $.math_symbol),
      alias('|]', $.math_shorthand),
    ),


    _math_common: $ => choice(
      $._math_term,
      $.align_point,
      alias('\\', $.linebreak),
      seq(alias($._math_statement, $.code), choice(';', $._line_end)),
    ),

    // `$f(#let x = 1;)$`: a statement in math ends at its semicolon.
    _math_statement: $ => seq('#', $._statement),

    _math_term: $ => choice($._math_unary, $.math_frac),

    // `a/b/c` is `(a/b)/c`; the denominator is one attachment-level operand.
    math_frac: $ => prec.left(MATH_PREC.frac, seq(
      field('numerator', $._math_term),
      '/',
      field('denominator', $._math_unary),
    )),

    _math_unary: $ => choice($._math_primary, $.math_attach, $.math_root),

    // `x_1^2`, `f'`, `x^a_b`: a base and any run of sub-, superscripts and
    // primes. Typst nests `x^a^b` and keeps a sub/superscript pair in one
    // node; a flat run parses everything it accepts.
    //
    // Warning: **the run is a right-recursive rule, not a `repeat1`.** The
    // helper rule tree-sitter makes for a repeat does not carry the
    // precedence, so inside `(…)` the `^` of `(s_i^8)` lost to closing the
    // group early and attached to the whole group.
    math_attach: $ => prec.right(MATH_PREC.attach, seq(
      field('base', $._math_primary),
      $._math_scripts,
    )),

    _math_scripts: $ => prec.right(MATH_PREC.attach, seq(
      choice(
        seq('_', field('bottom', $._math_script)),
        seq('^', field('top', $._math_script)),
        field('primes', $.primes),
      ),
      optional($._math_scripts),
    )),

    _math_script: $ => choice($._math_primary, $.math_root),

    math_root: $ => prec.right(seq(
      field('sign', alias(/[√∛∜]/, $.root_sign)),
      field('radicand', $._math_unary),
    )),

    _math_primary: $ => choice(
      $._math_juxtaposed,
      $._math_factorial,
      $.primes,
      $.math_letter,
      $.math_ident,
      $.math_number,
      $.math_shorthand,
      $.math_symbol,
      $.string,
      alias($._math_escape, $.escape),
      $.math_delimited,
      $.math_call,
      alias($._math_code, $.code),
    ),

    // A letter, escape or string written straight against a parenthesis is
    // one operand with it, as tight as a call (`math_expr_prec` in
    // `parser.rs`): `f(x)/2` divides `f(x)`, not `(x)`. The group is not a
    // node of its own, here as in Typst, where it is plain `Math`.
    _math_juxtaposed: $ => seq(
      choice($.math_letter, alias($._math_escape, $.escape), $.string),
      alias($._math_tight_group, $.math_delimited),
    ),

    _math_tight_group: $ => prec.right(seq(
      field('open', alias($._call_paren, '(')),
      repeat($._math_inner_item),
      optional(field('close', choice(')', '|]', alias($._math_close_char, $.math_delimiter)))),
    )),

    // `n!` written without a space binds tighter than anything:
    // `√n!` is the root of `n!`.
    _math_factorial: $ => prec.left(MATH_PREC.attach + 1, seq(
      $._math_primary,
      alias(token.immediate('!'), $.math_symbol),
    )),

    // `(a + b)`, `[0, 1)`, `⌈x⌉`, `[| x |]`. Any opening matches any closing,
    // and an unclosed group simply runs on (`math_delimited` in `parser.rs`).
    math_delimited: $ => prec.right(seq(
      field('open', choice('(', '[|', alias($._math_open_char, $.math_delimiter))),
      repeat($._math_inner_item),
      optional(field('close', choice(')', '|]', alias($._math_close_char, $.math_delimiter)))),
    )),

    _math_open_char: _ => token(new RegExp(`[${MATH_OPEN.replace(/[\[\]\\]/g, '\\$&')}]`)),

    _math_close_char: _ => token(new RegExp(`[${MATH_CLOSE.replace(/[\[\]\\]/g, '\\$&')}]`)),

    // `floor(x)`, `vec(1, 2)`, `mat(1, 2; 3, 4)`, `op("lim", limits: #true)`.
    math_call: $ => prec(MATH_PREC.attach, seq(
      field('function', $.math_ident),
      field('arguments', $.math_arguments),
    )),

    // Arguments are separated by `,` and rows by `;`, and any of them may be
    // empty. A name or a spread is only seen at the start of one.
    math_arguments: $ => seq(
      alias($._call_paren, '('),
      optional($._math_arg),
      repeat(seq(choice(',', ';'), optional($._math_arg))),
      ')',
    ),

    _math_arg: $ => choice(
      repeat1($._math_arg_item),
      alias($._math_named, $.named),
      alias($._math_spread, $.spread),
    ),

    _math_named: $ => seq(
      field('name', alias($._math_arg_name, $.identifier)),
      ':',
      repeat1($._math_arg_item),
    ),

    // `#x` inside math. Its calls and field accesses take only the scanner's
    // tokens, which come without a space before them: a plain `.` here would
    // make `$#x.$` a field access missing its name.
    // `mat(..#rows)`: an argument that starts with `..` is spread, unless
    // nothing follows the dots (`maybe_math_spread_arg` in `lexer.rs`).
    _math_spread: $ => seq(alias($._math_spread_dots, '..'), repeat1($._math_arg_item)),

    _math_code: $ => seq('#', choice($._math_postfix, alias($._math_contextual, $.contextual))),

    _math_postfix: $ => choice(
      $._atomic,
      alias($._math_code_call, $.call),
      alias($._math_code_field, $.field_access),
    ),

    _math_code_call: $ => prec(PREC.postfix, seq(
      field('function', $._math_postfix),
      field('arguments', $.arguments),
    )),

    _math_code_field: $ => prec(PREC.postfix, seq(
      field('object', $._math_postfix),
      alias($._field_dot, '.'),
      field('field', $.identifier),
    )),

    _math_contextual: $ => seq('context', field('body', $._math_postfix)),

    align_point: _ => '&',

    primes: _ => /'+/,

    // A single letter is a variable; a longer name is a symbol or function
    // and may carry fields (`arrow.r.long`). Letters inside a name exclude `_`,
    // so `x_1` is an attachment.
    math_letter: _ => /[\p{XID_Start}]/,

    math_ident: _ => token(seq(
      /[\p{XID_Start}][^\P{XID_Continue}_]+/,
      repeat(seq('.', /[\p{XID_Start}][^\P{XID_Continue}_]*/)),
    )),

    math_number: _ => /[0-9]+(\.[0-9]+)?/,

    math_shorthand: _ => choice(...MATH_SHORTHANDS),

    // Anything else is a symbol on its own. The negative precedence makes every
    // other math token win a tie.
    math_symbol: _ => token(prec(-1, /[^\s]/)),

    // A backslash before whitespace is a line break, and any whitespace counts.
    _math_escape: _ => token(new RegExp(`\\\\(u\\{[0-9a-zA-Z]*\\}|[^${SPACE}])`)),
  },
});
