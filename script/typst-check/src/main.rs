//! Typst's own parser as a judge of the grammar.
//!
//! `typst-check` reads one path per line on stdin and prints `ok <path>` or
//! `rejected <path>`: whether typst-syntax parses the file without a syntax
//! error.
//!
//! `typst-check --compare [N]` reads the same paths and, for every file Typst
//! accepts, collects the nodes of a few dozen kinds from Typst's tree and from
//! this grammar's, and reports where they differ: in number, in where they
//! start, and in where they start and end. Up to N files are named for each
//! kind.
//!
//! `typst-check --where KIND FILE` lists the nodes of one kind that only one of
//! the two trees has, by line and column.
//!
//! "No ERROR node" says the grammar accepted a file. This says whether it read
//! it the way Typst does: a heading that Typst sees and the grammar takes for
//! text passes the first test and fails this one.

use std::collections::BTreeMap;
use std::io::{BufRead, Write};

/// What is compared, by name: Typst's kinds on one side, the grammar's on the
/// other. A name maps to several kinds where one side splits what the other
/// keeps together.
///
/// Left out because the trees differ by design, not by mistake: attachments
/// (`x^a^b` is one `math_attach` here and two nested in Typst), delimited math
/// groups (Typst unwraps the parentheses of a fraction's operands), arrays,
/// dictionaries and parenthesized expressions (Typst parses a parameter list or
/// a destructuring pattern as such), and text (split at other places).
const MEASURES: &[(&str, &[&str], &[&str])] = &[
    ("heading", &["Heading"], &["heading"]),
    ("list_item", &["ListItem"], &["list_item"]),
    ("enum_item", &["EnumItem"], &["enum_item"]),
    ("term_item", &["TermItem"], &["term_item"]),
    ("strong", &["Strong"], &["strong"]),
    ("emph", &["Emph"], &["emph"]),
    ("equation", &["Equation"], &["equation"]),
    ("raw", &["Raw"], &["raw_span", "raw_block"]),
    ("url", &["Link"], &["url"]),
    ("label", &["Label"], &["label"]),
    ("reference", &["Ref"], &["reference"]),
    ("linebreak", &["Linebreak"], &["linebreak"]),
    ("escape", &["Escape"], &["escape"]),
    ("shorthand", &["Shorthand"], &["shorthand"]),
    ("parbreak", &["Parbreak"], &["parbreak"]),
    ("embedded", &["Hash"], &["#"]),
    ("call", &["FuncCall"], &["call"]),
    ("field_access", &["FieldAccess"], &["field_access"]),
    ("let_binding", &["LetBinding"], &["let_binding"]),
    ("set_rule", &["SetRule"], &["set_rule"]),
    ("show_rule", &["ShowRule"], &["show_rule"]),
    ("import", &["ModuleImport"], &["module_import"]),
    ("include", &["ModuleInclude"], &["module_include"]),
    ("closure", &["Closure"], &["closure", "let_function"]),
    ("conditional", &["Conditional"], &["conditional"]),
    ("for_loop", &["ForLoop"], &["for_loop"]),
    ("while_loop", &["WhileLoop"], &["while_loop"]),
    ("contextual", &["Contextual"], &["contextual"]),
    ("code_block", &["CodeBlock"], &["code_block"]),
    ("content_block", &["ContentBlock"], &["content_block"]),
    ("binary", &["Binary"], &["binary"]),
    ("destructuring", &["DestructAssignment"], &["destructuring_assignment"]),
    ("unary", &["Unary"], &["unary"]),
    ("string", &["Str"], &["string"]),
    ("number", &["Int", "Float", "Numeric"], &["int", "float", "numeric"]),
    ("math_call", &["MathCall"], &["math_call"]),
    ("math_frac", &["MathFrac"], &["math_frac"]),
    ("math_root", &["MathRoot"], &["math_root"]),
    ("align_point", &["MathAlignPoint"], &["align_point"]),
    ("math_shorthand", &["MathShorthand"], &["math_shorthand", "[|", "|]"]),
    ("primes", &["MathPrimes"], &["primes"]),
    ("raw_lang", &["RawLang"], &["raw_lang"]),
    ("named", &["Named", "Keyed"], &["named"]),
    ("spread", &["Spread"], &["spread"]),
    ("identifier", &["Ident"], &["identifier"]),
    ("break", &["LoopBreak"], &["break_statement"]),
    ("continue", &["LoopContinue"], &["continue_statement"]),
    ("return", &["FuncReturn"], &["return_statement"]),
    ("literal", &["Bool", "None", "Auto"], &["bool", "none", "auto"]),
];

/// Every node of every kind, as byte ranges.
type Spans = BTreeMap<String, Vec<(usize, usize)>>;

fn typst_spans(node: &typst_syntax::LinkedNode, spans: &mut Spans) {
    // `Debug` gives the variant name, which is what MEASURES spells.
    spans
        .entry(format!("{:?}", node.kind()))
        .or_default()
        .push((node.offset(), node.offset() + node.len()));
    for child in node.children() {
        typst_spans(&child, spans);
    }
}

fn tree_sitter_spans(root: tree_sitter::Node, spans: &mut Spans) {
    let mut stack = vec![root];
    while let Some(node) = stack.pop() {
        let range = (node.start_byte(), node.end_byte());
        let mut kind = node.kind();
        // Escapes inside strings are nodes here and part of the string token there.
        if kind == "escape" && node.parent().is_some_and(|p| p.kind() == "string") {
            kind = "string_escape";
        }
        // Typst wraps `let f(x) = …` in a closure that starts at the name; here
        // the binding holds the parameters itself.
        if kind == "let_binding" && node.child_by_field_name("parameters").is_some() {
            if let Some(name) = node.child_by_field_name("name") {
                spans.entry("let_function".into()).or_default().push((name.start_byte(), range.1));
            }
        }
        spans.entry(kind.into()).or_default().push(range);
        let mut cursor = node.walk();
        stack.extend(node.children(&mut cursor));
    }
}

fn collect(spans: &Spans, kinds: &[&str]) -> Vec<(usize, usize)> {
    let mut all: Vec<_> =
        kinds.iter().flat_map(|k| spans.get(*k).into_iter().flatten().copied()).collect();
    all.sort();
    all
}

fn line_col(text: &str, offset: usize) -> (usize, usize) {
    let before = &text[..offset];
    let line = before.matches('\n').count() + 1;
    let col = before.rsplit('\n').next().map_or(0, |l| l.chars().count()) + 1;
    (line, col)
}

fn parse_both(text: &str, parser: &mut tree_sitter::Parser) -> (Spans, Spans) {
    let root = typst_syntax::parse(text);
    let mut theirs = Spans::new();
    typst_spans(&typst_syntax::LinkedNode::new(&root), &mut theirs);
    let tree = parser.parse(text, None).expect("parsing");
    let mut ours = Spans::new();
    tree_sitter_spans(tree.root_node(), &mut ours);
    (theirs, ours)
}

fn where_kind(name: &str, path: &str, parser: &mut tree_sitter::Parser) {
    let (_, typst_kinds, ts_kinds) = MEASURES
        .iter()
        .find(|(n, _, _)| *n == name)
        .unwrap_or_else(|| panic!("no measure named {name}"));
    let text = std::fs::read_to_string(path).expect("reading the file");
    let (theirs, ours) = parse_both(&text, parser);
    let (a, b) = (collect(&theirs, typst_kinds), collect(&ours, ts_kinds));
    let show = |who: &str, (start, end): (usize, usize)| {
        let (l1, c1) = line_col(&text, start);
        let (l2, c2) = line_col(&text, end);
        println!("{who} {l1}:{c1}-{l2}:{c2}");
    };
    for span in a.iter().filter(|s| !b.contains(s)) {
        show("Typst only", *span);
    }
    for span in b.iter().filter(|s| !a.contains(s)) {
        show("here only ", *span);
    }
}

#[derive(Default)]
struct Row {
    /// Files where the number differs, where the starts do, where the spans do.
    count: usize,
    start: usize,
    span: usize,
    theirs: usize,
    ours: usize,
    examples: Vec<String>,
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut parser = tree_sitter::Parser::new();
    parser
        .set_language(&tree_sitter_typst::LANGUAGE.into())
        .expect("loading the grammar");

    if args.get(1).is_some_and(|a| a == "--where") {
        where_kind(&args[2], &args[3], &mut parser);
        return;
    }
    let compare = args.get(1).is_some_and(|a| a == "--compare");
    let examples: usize = args.get(2).and_then(|n| n.parse().ok()).unwrap_or(5);

    let stdout = std::io::stdout();
    let mut out = std::io::BufWriter::new(stdout.lock());

    let mut report: BTreeMap<&str, Row> = BTreeMap::new();
    let (mut compared, mut same_count, mut same_start, mut same_span) = (0, 0, 0, 0);

    for line in std::io::stdin().lock().lines() {
        let path = line.expect("reading stdin");
        let Ok(text) = std::fs::read_to_string(&path) else {
            if !compare {
                writeln!(out, "rejected {path}").unwrap();
            }
            continue;
        };
        let accepted = typst_syntax::parse(&text).errors_and_warnings().0.is_empty();
        if !compare {
            writeln!(out, "{} {path}", if accepted { "ok" } else { "rejected" }).unwrap();
            continue;
        }
        if !accepted {
            continue;
        }
        let (theirs, ours) = parse_both(&text, &mut parser);
        compared += 1;
        let (mut file_count, mut file_start, mut file_span) = (true, true, true);
        for (name, typst_kinds, ts_kinds) in MEASURES {
            let (a, b) = (collect(&theirs, typst_kinds), collect(&ours, ts_kinds));
            let row = report.entry(name).or_default();
            row.theirs += a.len();
            row.ours += b.len();
            let count = a.len() == b.len();
            let start = count && a.iter().zip(&b).all(|(x, y)| x.0 == y.0);
            let span = start && a == b;
            row.count += !count as usize;
            row.start += !start as usize;
            row.span += !span as usize;
            if !span && row.examples.len() < examples {
                let what = if !count { "number" } else if !start { "starts" } else { "ends" };
                row.examples.push(format!("{path} ({what}: Typst {}, here {})", a.len(), b.len()));
            }
            file_count &= count;
            file_start &= start;
            file_span &= span;
        }
        same_count += file_count as usize;
        same_start += file_start as usize;
        same_span += file_span as usize;
    }

    if compare {
        let pct = |n: usize| n as f64 * 100.0 / compared.max(1) as f64;
        let kinds = MEASURES.len();
        writeln!(out, "{compared} files Typst accepts, compared on {kinds} kinds of node:").unwrap();
        writeln!(out, "  {same_count} agree on how many of each ({:.2}%)", pct(same_count)).unwrap();
        writeln!(out, "  {same_start} also on where each starts ({:.2}%)", pct(same_start)).unwrap();
        writeln!(out, "  {same_span} also on where each ends ({:.2}%)\n", pct(same_span)).unwrap();
        writeln!(out, "{:<14} {:>9} {:>9}   files differing in: number starts ends", "kind", "Typst", "here")
            .unwrap();
        for (name, row) in &report {
            writeln!(
                out,
                "{name:<14} {:>9} {:>9}   {:>26} {:>6} {:>4}",
                row.theirs, row.ours, row.count, row.start, row.span
            )
            .unwrap();
            for example in &row.examples {
                writeln!(out, "    {example}").unwrap();
            }
        }
    }
}
