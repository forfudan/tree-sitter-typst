; **Scopes, definitions and references.**
;
; A code block, a content block, a closure and a for loop each open a scope,
; as they do in Typst.
;
; Warning: **a function `let` opens no scope here.** Its parameters belong in
; one and its name does not, and the name and the parameters are children of
; the same node. The parameters are defined in the scope around the binding,
; which is wrong only when a parameter shadows a name used after the binding.

(code_block) @local.scope
(content_block) @local.scope
(closure) @local.scope
(for_loop) @local.scope

(let_binding name: (identifier) @local.definition)
(let_binding pattern: (identifier) @local.definition)
(let_binding pattern: (_ (identifier) @local.definition))
(let_binding pattern: (_ (named value: (identifier) @local.definition)))
(let_binding pattern: (_ (spread value: (identifier) @local.definition)))

(parameters (identifier) @local.definition)
(parameters (named name: (identifier) @local.definition))
(parameters (spread value: (identifier) @local.definition))

(closure parameters: (identifier) @local.definition)
(closure parameters: (_ (identifier) @local.definition))
(closure parameters: (_ (named name: (identifier) @local.definition)))
(closure parameters: (_ (spread value: (identifier) @local.definition)))

(for_loop pattern: (identifier) @local.definition)
(for_loop pattern: (_ (identifier) @local.definition))

(module_import alias: (identifier) @local.definition)
(renamed_import_item alias: (identifier) @local.definition)

(identifier) @local.reference
