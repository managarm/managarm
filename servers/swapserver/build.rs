fn main() -> Result<(), Box<dyn std::error::Error>> {
    bragi_build::generate_bindings("../../protocols/swap/swap.bragi", "swap.rs")?;
    Ok(())
}
