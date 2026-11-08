# tree-sitter-typst

A [tree-sitter](https://tree-sitter.github.io) grammar for
[Typst](https://typst.app), written from scratch rather than forked from an
existing Typst grammar.

## Why from scratch

Typst is three languages that hand over to each other mid-line. Markup, code
after `#` and inside `{}`, math inside `$…$`: the same `-` is a list marker, a
shorthand or text in markup, an operator in code and a shorthand in math, and
which one it is depends on what came before it on the line. Typst's own lexer
looks around a token to decide — whether a `*` stands inside a word, whether a
`-` starts a line, whether `(` follows a name without a space — and its parser
ends a list item at the first line indented no deeper than the marker.

This grammar is written against that lexer and parser, `crates/typst-syntax` in
[typst/typst](https://github.com/typst/typst) (`lexer.rs`, `parser.rs`, `set.rs`,
`kind.rs`) as of `84cca40` (2026-10-28, version 0.15.1 with unreleased changes),
and checked against Typst written by the people who write Typst: the compiler's test suite and documentation, and the newest version
of every package in [typst/packages](https://github.com/typst/packages).

Markup is lexed entirely by the external scanner (`src/scanner.c`), and so is
everything in code that whitespace decides: `#f(x)` calls and `#f (x)` is a name
followed by text, `#x.y` is a field and `#x. Y` ends a sentence, a statement
after a hash ends at its line and one in a code block goes on when the next line
starts with `.` or `else`.

## What it covers

- **Markup**: headings (a trailing label stays outside, as in Typst), bullet,
  numbered and term lists with their indentation, strong and emphasis (with
  Typst's rule for `*` and `_` inside words, which East Asian scripts do not
  form), raw text and raw blocks with language tags, links, labels, references
  with supplements, escapes, shorthands, line and paragraph breaks, smart
  quotes, and `[`…`]` in text, which Typst counts so that the inner `]` of
  `#[a [b] c]` does not close the block.
- **Code**: `let` (with parameters, defaults, sinks and destructuring), `set`
  (with `if`), `show` (with and without a selector), `import` (packages,
  `as`, item lists, nested paths), `include`, `if`/`else`, `for`, `while`,
  `break`, `continue`, `return`, `context`, closures, method chains, field
  access, calls with trailing content blocks, named and spread arguments,
  arrays, dictionaries, destructuring assignment, strings with escapes, numbers
  with units (`pt`, `mm`, `cm`, `in`, `em`, `fr`, `deg`, `rad`, `%`), labels, and
  `//` and nested `/* */` comments anywhere.
- **Math**: fractions, sub- and superscripts and primes, roots, alignment
  points, line breaks, shorthands, symbols with fields (`arrow.r.long`), calls
  with `,`/`;` arguments and named arguments, delimiters matched as Typst
  matches them (any opening with any closing, unclosed groups allowed), strings,
  escapes and code with `#`.

The queries give helix-compatible capture names: `highlights.scm`,
`injections.scm` (a raw block's tag names its language), `locals.scm` and
`tags.scm` (headings and `let` bindings, for an outline).

## The number

```
script/baseline.sh          # how much of the corpus parses with no ERROR node,
                            # and how much of it is read the way Typst reads it
script/baseline.sh --why    # and where it is not
```

Point it at your own checkouts with `TYPST_SRC=/path/to/typst` and
`PACKAGES_SRC=/path/to/packages` (a sparse checkout of the packages' `.typ`
files is enough). It needs `tree-sitter`, `cargo` and `python3`.

The judge is Typst's own parser: `script/typst-check` is typst-syntax 0.15.1 in
a small program. A file it rejects is not Typst, and the first number leaves it
out. On typst/typst at `84cca40` (2026-10-28) and typst/packages at the newest
version of each of 1741 packages:

```
tests        3822 files,   153 rejected by Typst; of the   3669 Typst accepts    0 fail: 100.00%   (all files:  97.70%)
repo           75 files,     0 rejected by Typst; of the     75 Typst accepts    0 fail: 100.00%   (all files: 100.00%)
packages    14115 files,     5 rejected by Typst; of the  14110 Typst accepts    0 fail: 100.00%   (all files: 100.00%)
total       18012 files,   158 rejected by Typst; of the  17854 Typst accepts    0 fail: 100.00%   (all files:  99.51%)
```

The test suite is cut up first: a file there holds dozens of tests under
`--- name ---` lines, and many are meant to fail, so each test becomes a file
of its own and one deliberate error does not hide the rest of its file. The 153
it rejects are those deliberate errors.

No ERROR node is not the same as being right: a heading taken for text parses
fine. So the judge also parses every file it accepts into Typst's own tree and
compares it with this grammar's, on 49 kinds of node — headings, items, strong,
raw, calls, rules, closures, blocks, operators, fractions and the rest:

```
17854 files Typst accepts, compared on 49 kinds of node:
  17854 agree on how many of each (100.00%)
  17849 also on where each starts (99.97%)
  17843 also on where each ends (99.94%)
```

The eleven files that differ are listed by `--why`; they are the deliberate
differences below. The kinds left out of the comparison are left out because
the trees differ by design (`script/typst-check/src/main.rs` says which). Read a fall in any of these numbers, not their distance from
100.

## Where it differs from Typst on purpose

- **Trees are tree-sitter trees.** Typst's tree keeps every token; this one
  names what an editor asks about. An array, a dictionary and a parameter list
  that look alike are parsed alike (Typst backtracks to tell them apart), so
  `(a, b: 1) => …` has an `array` for parameters. `x_a^b` is one `math_attach`,
  and so is `x^a^b`, where Typst nests the second.
- **A comment right after a heading's or an item's text** (`= h/**/`,
  `- a ~// note`) and comment lines less indented than the list item around
  them end the structure a little later or earlier than Typst ends it; they
  are the only `ends` differences.
- **A few math groupings**: Typst treats a symbol straight against `(` as one
  operand (`℘()/2`); here only letters, strings and escapes are.
- **An unclosed `*` or `_` is not an error.** Typst gives up on it at the
  paragraph break, the `]` of its block or the end, and reports it; here it
  simply ends there, so that typing a `*` does not turn the rest of the
  document bold. Other errors are tree-sitter's to recover from.

## Layout

| | |
| --- | --- |
| `grammar.js` | the rules. Typed via `tree-sitter-cli`'s own `dsl.d.ts`, which is what `// @ts-check` at the top buys |
| `src/scanner.c` | the external scanner: all of markup, and the code tokens whitespace decides. **The comment at the top is the contract** with tree-sitter's lexer and parser, and the reason markup is external all the way down |
| `src/unicode.h` | Unicode classes for the scanner (alphanumeric, numeric, XID), generated by `script/unicode.py` |
| `src/parser.c` | generated, and **committed**, so everything downstream builds without node or the CLI |
| `queries/` | highlights, injections, locals, tags |
| `test/corpus/` | `tree-sitter test` cases. The baseline says how far the grammar reaches; these pin the places it has already been wrong. Warning: `-u` rewrites **every** corpus file it reads |
| `test/highlight/` | highlight assertions, run by the same `tree-sitter test` |
| `script/baseline.sh` | the numbers above |
| `script/typst-check/` | Typst's parser as the judge: `ok`/`rejected` per file, `--compare` for the trees, `--where KIND FILE` to find the node that differs |
| `bindings/rust/` | the crate: `LANGUAGE`, the four queries and `NODE_TYPES` |

## Building

```
npm install -g tree-sitter-cli
tree-sitter generate --abi 15
tree-sitter parse some.typ
```

Warning: keep `--abi 15`. The committed `parser.c` is ABI 15 so that the tree-sitter
0.25 runtime (ABI 13 to 15) can load it; a CLI that defaults to a newer ABI would
produce a parser that builds and then refuses to load.

As a Rust dependency:

```toml
tree-sitter-typst = { git = "https://github.com/forfudan/tree-sitter-typst", tag = "v0.1.0" }
```

## Licence

MIT. No code is taken from any other Typst grammar; the corpus and the reference
parser are Typst's and its package authors', under their own licences, and are
read, not copied.
