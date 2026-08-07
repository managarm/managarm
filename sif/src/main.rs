use anyhow::{Result, bail};

mod acpi;
mod io;
mod pci;

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

        pci::publish_devices().await?;

        println!("sif: published PCI devices");

        std::future::pending::<Result<()>>().await
    })?
}
