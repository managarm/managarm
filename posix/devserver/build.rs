fn main() -> Result<(), Box<dyn std::error::Error>> {
    bragi_build::generate_bindings("../../protocols/posix/devserver.bragi", "devserver.rs")?;
    Ok(())
}
