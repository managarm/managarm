use anyhow::{Result, bail};

mod acpi;
mod io;
// Only x86 has an ISA bus (and hence ISA IRQs).
#[cfg(target_arch = "x86_64")]
mod isa;
mod pci;
mod uacpi;

fn main() -> Result<()> {
    hel::block_on(async {
        let cmdline = managarm::kerncfg::get_cmdline().await?;
        if !cmdline.split_ascii_whitespace().any(|opt| opt == "sif") {
            println!("sif: disabled on the kernel command line");
            return Ok(());
        }
        println!("sif: enabled");

        let rsdp = managarm::kerncfg::get_acpi_rsdp().await?;
        if rsdp == 0 {
            bail!("sif: kernel reported no ACPI RSDP");
        }
        acpi::set_rsdp(rsdp);
        acpi::uacpi_init()?;

        println!("sif: uACPI initialized");

        // Configure the ISA IRQs before the PCI links to match thor's ordering.
        #[cfg(target_arch = "x86_64")]
        isa::configure_isa_irqs();

        pci::publish_devices().await?;

        println!("sif: published PCI devices");

        std::future::pending::<Result<()>>().await
    })?
}
