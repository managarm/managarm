use std::path::PathBuf;

fn main() {
    let installdir = PathBuf::from(pkg_config::get_variable("uacpi", "installdir").unwrap());
    let includedir = pkg_config::get_variable("uacpi", "includedir").unwrap();
    let sourcefiles = pkg_config::get_variable("uacpi", "sourcefiles").unwrap();

    let mut b = cc::Build::new();
    for src in sourcefiles.split(';').filter(|s| !s.is_empty()) {
        b.file(installdir.join(src));
    }

    b.include(&includedir).pic(true);

    b.compile("uacpi");

    let bindings = bindgen::builder()
        .wrap_unsafe_ops(true)
        .derive_default(true)
        .derive_debug(true)
        .prepend_enum_name(false)
        .header("src/wrapper.h")
        .clang_arg(format!("-I{includedir}"))
        .formatter(bindgen::Formatter::Rustfmt)
        .generate()
        .expect("Unable to generate bindings!");

    let out_path = PathBuf::from(std::env::var("OUT_DIR").unwrap());

    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Unable to write bindings!");
}
