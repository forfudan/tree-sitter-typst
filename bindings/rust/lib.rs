//! This crate provides Typst language support for the [tree-sitter] parsing library.
//!
//! [tree-sitter]: https://tree-sitter.github.io/

use tree_sitter_language::LanguageFn;

extern "C" {
    fn tree_sitter_typst() -> *const ();
}

/// The tree-sitter [`LanguageFn`] for this grammar.
pub const LANGUAGE: LanguageFn = unsafe { LanguageFn::from_raw(tree_sitter_typst) };

/// The syntax highlighting query, with capture names an editor such as helix
/// already has colours for.
pub const HIGHLIGHTS_QUERY: &str = include_str!("../../queries/highlights.scm");

/// The injection query: the language of a raw block is its tag.
pub const INJECTIONS_QUERY: &str = include_str!("../../queries/injections.scm");

/// The locals query: scopes, definitions and references.
pub const LOCALS_QUERY: &str = include_str!("../../queries/locals.scm");

/// The tagging query: what counts as a definition, for an outline and for
/// stepping between them.
pub const TAGS_QUERY: &str = include_str!("../../queries/tags.scm");

/// The content of the [`node-types.json`][] file for this grammar.
///
/// [`node-types.json`]: https://tree-sitter.github.io/tree-sitter/using-parsers/6-static-node-types
pub const NODE_TYPES: &str = include_str!("../../src/node-types.json");

#[cfg(test)]
mod tests {
    use streaming_iterator::StreamingIterator;

    fn language() -> tree_sitter::Language {
        super::LANGUAGE.into()
    }

    fn parse(source: &str) -> tree_sitter::Tree {
        let mut parser = tree_sitter::Parser::new();
        parser.set_language(&language()).expect("loading the Typst grammar");
        parser.parse(source, None).unwrap()
    }

    /// Every capture of `query` over `source`, as (capture name, text).
    fn captures<'s>(query: &str, source: &'s str) -> Vec<(String, &'s str)> {
        let query = tree_sitter::Query::new(&language(), query).expect("the query compiles");
        let tree = parse(source);
        let mut cursor = tree_sitter::QueryCursor::new();
        let mut found = Vec::new();
        let mut matches = cursor.matches(&query, tree.root_node(), source.as_bytes());
        while let Some(m) = matches.next() {
            for capture in m.captures {
                let name = query.capture_names()[capture.index as usize].to_string();
                found.push((name, capture.node.utf8_text(source.as_bytes()).unwrap()));
            }
        }
        found
    }

    #[test]
    fn the_grammar_loads() {
        let tree = parse("= Title\n#let x = 1\nSome *text* and $x^2$.\n");
        assert!(!tree.root_node().has_error(), "{}", tree.root_node().to_sexp());
    }

    #[test]
    fn the_highlights_query_compiles_and_names_a_call() {
        let found = captures(super::HIGHLIGHTS_QUERY, "#text(fill: red)[hi]\n");
        assert!(found.contains(&("function".into(), "text")), "{found:?}");
        assert!(found.contains(&("variable.parameter".into(), "fill")), "{found:?}");
    }

    #[test]
    fn the_injections_query_finds_the_language_of_a_raw_block() {
        let found = captures(super::INJECTIONS_QUERY, "```rust\nfn main() {}\n```\n");
        assert!(found.contains(&("injection.language".into(), "rust")), "{found:?}");
    }

    #[test]
    fn the_locals_query_compiles() {
        let found = captures(super::LOCALS_QUERY, "#let f(x) = x + 1\n");
        assert!(found.contains(&("local.definition".into(), "x")), "{found:?}");
    }

    #[test]
    fn the_tags_query_finds_headings_and_bindings() {
        let source = "= Intro\n#let f(x) = x\n#let width = 2pt\n== Details <d>\n";
        let mut names: Vec<_> = captures(super::TAGS_QUERY, source)
            .into_iter()
            .filter(|(name, _)| name == "name")
            .map(|(_, text)| text)
            .collect();
        names.sort();
        assert_eq!(names, ["= Intro", "== Details", "f", "width"]);
    }
}
