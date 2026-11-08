; **A raw block's tag names its language**: ```rust … ``` is Rust.
;
; Typst reads the tag up to the end of an identifier (`raw_lang_tag` in
; `lexer.rs`), so ```c++ is tagged `c` and `++` starts the text; an editor
; then highlights it as C, which is what Typst itself does with it.
;
; Typst's own tags, `typ` for markup and `typc` for code, find this grammar
; through the `injection-regex` in `tree-sitter.json`. There is no entry point
; for code mode, so a `typc` block is read as markup; such a block is usually
; one expression, which reads the same after a hash.

(raw_block
  (raw_lang) @injection.language
  (raw_text) @injection.content)

((line_comment) @injection.content
  (#set! injection.language "comment"))

((block_comment) @injection.content
  (#set! injection.language "comment"))
