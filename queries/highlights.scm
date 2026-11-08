; **Syntax highlighting**, with capture names helix themes already colour.
;
; Warning: **the last pattern that matches a node wins**, in helix and in
; `tree-sitter highlight` alike. General patterns come first and particular
; ones after: plain identifiers before a call's name, the multiplication
; operator before the strong delimiter it is spelled like.

; ---- code: the general case --------------------------------------------------

(identifier) @variable

[
  "+" "-" "−" "*" "/" "==" "!=" "<" "<=" ">" ">=" "="
  "+=" "-=" "−=" "*=" "/=" "=>" ".."
] @operator

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" "." ":"] @punctuation.delimiter
["#" "$"] @punctuation.special

"let" @keyword.storage
["set" "show" "context"] @keyword
["if" "else"] @keyword.control.conditional
["for" "while" (break_statement) (continue_statement)] @keyword.control.repeat
"return" @keyword.control.return
["import" "include" "as"] @keyword.control.import
["and" "or" "not" "in"] @keyword.operator
(for_loop "in" @keyword.control.repeat)

(string) @string
(int) @constant.numeric.integer
[(float) (numeric)] @constant.numeric.float
(bool) @constant.builtin.boolean
[(none) (auto)] @constant.builtin

(line_comment) @comment.line
(block_comment) @comment.block
(shebang) @keyword.directive

; ---- code: names in particular places ----------------------------------------

; Warning: **a field has no capture of its own.** Typst draws a field in the
; colour of the name before its dot (`highlight_ident` in typst-syntax), so
; `it.body` and a dictionary's keys are plain names, as `(identifier)` above
; makes them; only a field that is called or shown is a function, below.
(arguments (named name: (identifier) @variable.parameter))

(parameters (identifier) @variable.parameter)
(parameters (named name: (identifier) @variable.parameter))
(parameters (spread value: (identifier) @variable.parameter))
(closure parameters: (identifier) @variable.parameter)
(closure parameters: (_ (identifier) @variable.parameter))
(closure parameters: (_ (named name: (identifier) @variable.parameter)))
(closure parameters: (_ (spread value: (identifier) @variable.parameter)))

(module_import alias: (identifier) @namespace)
(module_import source: (string) @string.special.path)
(module_include source: (string) @string.special.path)

(let_binding name: (identifier) @function)
(let_binding
  pattern: (identifier) @function
  value: (closure))
(call function: (identifier) @function)
(call function: (field_access field: (identifier) @function.method))
(set_rule target: (identifier) @function)
(set_rule target: (field_access field: (identifier) @function))
; A show rule's name right before or after its colon is a function, as Typst
; has it: `heading` in `show heading:`, `caption` in `show figure.caption:`,
; `template` in `show: template`. In `show figure.where(…)` only the call is.
(show_rule selector: (identifier) @function)
(show_rule selector: (field_access field: (identifier) @function))
(show_rule transform: (identifier) @function)

; ---- markup --------------------------------------------------------------------

((heading (heading_marker) @_m) @markup.heading.1 (#eq? @_m "="))
((heading (heading_marker) @_m) @markup.heading.2 (#eq? @_m "=="))
((heading (heading_marker) @_m) @markup.heading.3 (#eq? @_m "==="))
((heading (heading_marker) @_m) @markup.heading.4 (#eq? @_m "===="))
((heading (heading_marker) @_m) @markup.heading.5 (#eq? @_m "====="))
((heading (heading_marker) @_m) @markup.heading.6 (#match? @_m "^======+$"))
(heading_marker) @markup.heading.marker

(strong) @markup.bold
(emph) @markup.italic
(strong "*" @punctuation.delimiter)
(emph "_" @punctuation.delimiter)

(raw_span) @markup.raw.inline
(raw_block) @markup.raw.block
(raw_block (raw_lang) @label)

(url) @markup.link.url
(reference) @markup.link.label
(label) @label

(list_marker) @markup.list.unnumbered
(enum_marker) @markup.list.numbered
(term_marker) @markup.list.unnumbered
(term) @markup.bold

(escape) @constant.character.escape
(linebreak) @constant.character.escape
(shorthand) @constant.character

; ---- math ----------------------------------------------------------------------

(math_letter) @variable
(math_ident) @constant.builtin
(math_number) @constant.numeric
[(math_shorthand) (math_symbol) (primes) (root_sign)] @operator
(math_attach ["_" "^"] @operator)
(math_frac "/" @operator)
(math_delimited ["(" ")" "[|" "|]"] @punctuation.bracket)
(math_delimiter) @punctuation.bracket
(align_point) @punctuation.special
(math_call function: (math_ident) @function)
(math_arguments (named name: (identifier) @variable.parameter))
