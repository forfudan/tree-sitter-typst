; **Syntax highlighting**, with capture names helix themes already colour.
;
; Warning: **the first pattern that matches a node wins** (helix and
; tree-sitter-highlight). Specific patterns come before general ones: a call's
; name before plain identifiers, a strong delimiter before the multiplication
; operator it is spelled like.

; ---- markup ------------------------------------------------------------------

((heading (heading_marker) @_m) @markup.heading.1 (#eq? @_m "="))
((heading (heading_marker) @_m) @markup.heading.2 (#eq? @_m "=="))
((heading (heading_marker) @_m) @markup.heading.3 (#eq? @_m "==="))
((heading (heading_marker) @_m) @markup.heading.4 (#eq? @_m "===="))
((heading (heading_marker) @_m) @markup.heading.5 (#eq? @_m "====="))
((heading (heading_marker) @_m) @markup.heading.6 (#match? @_m "^======+$"))
(heading_marker) @markup.heading.marker

(strong) @markup.bold
(emph) @markup.italic

(raw_span) @markup.raw.inline
(raw_block (raw_lang) @label)
(raw_block) @markup.raw.block

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

; ---- math --------------------------------------------------------------------

(equation "$" @punctuation.special)

(math_call function: (math_ident) @function)
(math_arguments (named name: (identifier) @variable.parameter))
(math_ident) @constant.builtin
(math_letter) @variable
(math_number) @constant.numeric
(math_shorthand) @operator
(math_symbol) @operator
(primes) @operator
(root_sign) @operator
(align_point) @punctuation.special
(math_attach ["_" "^"] @operator)
(math_frac "/" @operator)
(math_delimited ["(" ")" "[|" "|]"] @punctuation.bracket)
(math_delimiter) @punctuation.bracket

; ---- code --------------------------------------------------------------------

(code "#" @punctuation.special)

(line_comment) @comment.line
(block_comment) @comment.block
(shebang) @keyword.directive

; Names, from the most particular place to the least.
(call function: (identifier) @function)
(call function: (field_access field: (identifier) @function.method))
(set_rule target: (identifier) @function)
(set_rule target: (field_access field: (identifier) @function))
(show_rule selector: (identifier) @function)
(show_rule selector: (call function: (field_access object: (identifier) @function)))
(let_binding name: (identifier) @function)
(let_binding pattern: (identifier) @function
  value: (closure))

(parameters (identifier) @variable.parameter)
(parameters (named name: (identifier) @variable.parameter))
(parameters (spread value: (identifier) @variable.parameter))
(closure parameters: (identifier) @variable.parameter)
(closure parameters: (_ (identifier) @variable.parameter))
(closure parameters: (_ (named name: (identifier) @variable.parameter)))
(closure parameters: (_ (spread value: (identifier) @variable.parameter)))

(arguments (named name: (identifier) @variable.parameter))
(dict (named name: (identifier) @variable.other.member))
(field_access field: (identifier) @variable.other.member)

(module_import alias: (identifier) @namespace)

(identifier) @variable

(module_import source: (string) @string.special.path)
(module_include source: (string) @string.special.path)
(string) @string
(int) @constant.numeric.integer
[(float) (numeric)] @constant.numeric.float
(bool) @constant.builtin.boolean
[(none) (auto)] @constant.builtin

"let" @keyword.storage
["set" "show" "context"] @keyword
["if" "else"] @keyword.control.conditional
(for_loop "in" @keyword.control.repeat)
["for" "while" (break_statement) (continue_statement)] @keyword.control.repeat
"return" @keyword.control.return
["import" "include" "as"] @keyword.control.import
["and" "or" "not" "in"] @keyword.operator

(strong "*" @punctuation.delimiter)
(emph "_" @punctuation.delimiter)

[
  "+" "-" "−" "*" "/" "==" "!=" "<" "<=" ">" ">=" "="
  "+=" "-=" "−=" "*=" "/=" "=>" ".."
] @operator

["(" ")" "[" "]" "{" "}"] @punctuation.bracket
["," ";" "." ":"] @punctuation.delimiter
"$" @punctuation.special
