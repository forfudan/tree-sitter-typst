; **A raw block's tag names its language**: ```rust … ``` is Rust.
;
; Typst reads the tag up to the end of an identifier (`raw_lang_tag` in
; `lexer.rs`), so ```c++ is tagged `c` and `++` starts the text; an editor
; then highlights it as C, which is what Typst itself does with it.
;
; `typ` and `typc` are Typst's own tags and come first, so that they are not
; looked up as languages of those names. A code-mode block (`typc`) has no
; entry point of its own in this grammar, so it is read as markup too; such a
; block is usually one expression, which reads the same after a hash.

((raw_block
  (raw_lang) @_lang
  (raw_text) @injection.content)
  (#any-of? @_lang "typ" "typc")
  (#set! injection.language "typst"))

(raw_block
  (raw_lang) @injection.language
  (raw_text) @injection.content)

((line_comment) @injection.content
  (#set! injection.language "comment"))

((block_comment) @injection.content
  (#set! injection.language "comment"))
