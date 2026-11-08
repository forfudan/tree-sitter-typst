; **What counts as a definition**: an outline, and a key that steps between them.
;
; The capture names are tree-sitter's own tagging convention (`@definition.*`
; with the name under `@name`), so anything that already reads a `tags.scm`
; reads this one.
;
; A heading is the outline of a document, so it is a definition too. Typst
; gives a heading's text no node of its own, so the whole heading is its name,
; marker included, which also says how deep it sits.
;
; Warning: **`let f = x => …` is a constant here, not a function.** Telling
; them apart needs "the value is a closure" in one pattern and "the value is
; anything else" in another, and a query has no "anything else": the plain
; pattern would capture the closure binding a second time.

(heading) @name @definition.section

(let_binding
  name: (identifier) @name) @definition.function

(let_binding
  pattern: (identifier) @name) @definition.constant
