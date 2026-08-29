use std::path::PathBuf;

use bindgen::callbacks::{DeriveInfo, ParseCallbacks};

/// ACPI structures that consumers parse out of raw table bytes.
const ZEROCOPY_STRUCTS: &[&str] = &[
    "acpi_entry_hdr",
    "acpi_madt_interrupt_source_override",
    "acpi_sdt_hdr",
];

/// Lets consumers read [`ZEROCOPY_STRUCTS`] from byte slices without unsafe code.
///
/// The derives cannot be applied to all of the bindings: zerocopy rejects the structures that
/// contain pointers, unions or flexible array members.
#[derive(Debug)]
struct ZerocopyDerives;

impl ParseCallbacks for ZerocopyDerives {
    fn add_derives(&self, info: &DeriveInfo<'_>) -> Vec<String> {
        if !ZEROCOPY_STRUCTS.contains(&info.name) {
            return Vec::new();
        }
        ["FromBytes", "IntoBytes", "Immutable", "KnownLayout"]
            .iter()
            .map(|derive| format!("zerocopy::{derive}"))
            .collect()
    }
}

fn main() {
    let installdir = PathBuf::from(pkg_config::get_variable("uacpi", "installdir").unwrap());
    let includedir = pkg_config::get_variable("uacpi", "includedir").unwrap();
    let sourcefiles = pkg_config::get_variable("uacpi", "sourcefiles").unwrap();

    let mut b = cc::Build::new();
    for src in sourcefiles.split(';').filter(|s| !s.is_empty()) {
        let path = installdir.join(src);
        println!("cargo:rerun-if-changed={}", path.display());
        b.file(path);
    }

    b.include(&includedir).pic(true);

    b.compile("uacpi");

    // pkg_config emits rerun-if-env-changed, which turns off cargo's default of scanning
    // the entire package for changes, and neither cc nor bindgen track their own inputs.
    // Cargo scans rerun-if-changed directories recursively, hence includedir covers all headers.
    println!("cargo:rerun-if-changed={includedir}");
    println!("cargo:rerun-if-changed=src/wrapper.h");

    let bindings = bindgen::builder()
        .wrap_unsafe_ops(true)
        .derive_default(true)
        .derive_debug(true)
        .prepend_enum_name(false)
        .parse_callbacks(Box::new(ZerocopyDerives))
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
