fn main() -> Result<(), Box<dyn std::error::Error>> {
    for protocol in ["clock", "fs", "hw", "mbus", "posix"] {
        let path = format!("../../protocols/{protocol}/{protocol}.bragi");
        let out_path = format!("{protocol}.rs");

        bragi_build::generate_bindings(path, &out_path)?;
    }

    Ok(())
}
