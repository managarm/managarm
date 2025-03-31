use std::path::{Path, PathBuf};

fn main() {
    let hel_include_path = Path::new("../../hel/include");

    cc::Build::new()
        .cpp(true)
        .include(hel_include_path)
        .flag("-fkeep-inline-functions")
        .file("hel.cpp")
        .compile("hel");

    let bindings = bindgen::Builder::default()
        .clang_arg(format!("-I{}", hel_include_path.display()))
        .header("wrapper.hpp")
        .parse_callbacks(Box::new(bindgen::CargoCallbacks::new()))
        .prepend_enum_name(false)
        .generate()
        .expect("Unable to generate bindings");

    let out_path = PathBuf::from(std::env::var("OUT_DIR").unwrap());

    bindings
        .write_to_file(out_path.join("bindings.rs"))
        .expect("Couldn't write bindings!");
}
