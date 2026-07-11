fn main() -> Result<(), Box<dyn std::error::Error>> {
    bragi_build::generate_bindings("../../protocols/kernlet/kernlet.bragi", "kernlet.rs")?;
    Ok(())
}
