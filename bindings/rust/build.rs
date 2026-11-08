fn main() {
    let src_dir = std::path::Path::new("src");
    let mut c_config = cc::Build::new();
    c_config.std("c11").include(src_dir);
    // Warning: **MSVC reads a source file in the system code page unless told
    // otherwise**, so the UTF-8 node names in parser.c (`−`, `…`, `≠`) come
    // out as other bytes and a query naming them fails to compile.
    if std::env::var("CARGO_CFG_TARGET_ENV").as_deref() == Ok("msvc") {
        c_config.flag("-utf-8");
    }
    for file in ["parser.c", "scanner.c"] {
        let path = src_dir.join(file);
        c_config.file(&path);
        println!("cargo:rerun-if-changed={}", path.to_str().unwrap());
    }
    c_config.compile("tree-sitter-typst");
}
