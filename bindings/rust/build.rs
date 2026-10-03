// Compiles the C library that lives two folders up.
fn main() {
    let root = std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    cc::Build::new()
        .file(root.join("yapf.c"))
        .include(&root)
        .flag_if_supported("-std=c99")
        .opt_level(2)
        .compile("yapf");
    println!("cargo:rerun-if-changed={}", root.join("yapf.c").display());
    println!("cargo:rerun-if-changed={}", root.join("yapf.h").display());
}
