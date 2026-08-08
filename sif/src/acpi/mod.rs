pub mod glue;

use std::sync::atomic::{AtomicU64, Ordering};

use anyhow::{Result, anyhow};

use uacpi_sys::uacpi_status;

pub(crate) const PAGE_SIZE: usize = 0x1000;
pub(crate) const PAGE_MASK: usize = PAGE_SIZE - 1;

pub(crate) static RSDP: AtomicU64 = AtomicU64::new(0);

pub fn set_rsdp(addr: u64) {
    RSDP.store(addr, Ordering::Relaxed);
}

fn check(status: uacpi_status, what: &str) -> Result<()> {
    if status == uacpi_sys::UACPI_STATUS_OK {
        Ok(())
    } else {
        Err(anyhow!("{what} failed: uacpi status {status:?}"))
    }
}

pub fn uacpi_init() -> Result<()> {
    unsafe {
        check(uacpi_sys::uacpi_initialize(0), "uacpi_initialize")?;
        crate::pci::config::discover_config_spaces()?;
        check(uacpi_sys::uacpi_namespace_load(), "uacpi_namespace_load")?;
        check(
            uacpi_sys::uacpi_set_interrupt_model(uacpi_sys::UACPI_INTERRUPT_MODEL_IOAPIC),
            "uacpi_set_interrupt_model",
        )?;
        check(
            uacpi_sys::uacpi_namespace_initialize(),
            "uacpi_namespace_initialize",
        )?;
    }

    Ok(())
}
