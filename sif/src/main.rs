use anyhow::{Result, bail};

mod acpi;
mod dt;
mod entity;
// Only x86 has an ISA bus (and hence ISA IRQs).
#[cfg(target_arch = "x86_64")]
mod isa;
mod pci;
mod uacpi;

pub(crate) fn leak<T>(value: T) -> &'static T {
    Box::leak(Box::new(value))
}

fn main() -> Result<()> {
    hel::block_on(async {
        let cmdline = managarm::kerncfg::get_cmdline().await?;
        if !cmdline.split_ascii_whitespace().any(|opt| opt == "sif") {
            println!("sif: disabled on the kernel command line");
            return Ok(());
        }
        println!("sif: enabled");

        let rsdp = managarm::kerncfg::get_acpi_rsdp().await?;
        if rsdp != 0 {
            acpi::set_rsdp(rsdp);
            acpi::configure_log_level(&cmdline);
            acpi::uacpi_init()?;

            println!("sif: uACPI initialized");

            // Configure the ISA IRQs before the PCI links to match thor's ordering.
            #[cfg(target_arch = "x86_64")]
            isa::configure_isa_irqs();
        } else {
            let (address, size) = managarm::kerncfg::get_device_tree().await?;
            if address == 0 {
                bail!("sif: kernel reported neither an ACPI RSDP nor a device tree");
            }
            dt::node::init(address, size)?;
            dt::irq::init();
        }

        pci::publish_devices().await?;

        println!("sif: published PCI devices");

        if acpi::has_rsdp() {
            acpi::ps2::publish().await?;
        }

        std::future::pending::<Result<()>>().await
    })?
}
