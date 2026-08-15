pub mod glue;

use std::sync::atomic::{AtomicU64, Ordering};

use anyhow::{Result, anyhow};

use uacpi_sys::uacpi_status;

pub(crate) const PAGE_SIZE: usize = 0x1000;
pub(crate) const PAGE_MASK: usize = PAGE_SIZE - 1;

pub(crate) static RSDP: AtomicU64 = AtomicU64::new(0);

#[cfg(target_arch = "x86_64")]
const INTERRUPT_MODEL: uacpi_sys::uacpi_interrupt_model = uacpi_sys::UACPI_INTERRUPT_MODEL_IOAPIC;
#[cfg(target_arch = "aarch64")]
const INTERRUPT_MODEL: uacpi_sys::uacpi_interrupt_model = uacpi_sys::UACPI_INTERRUPT_MODEL_GIC;
#[cfg(target_arch = "riscv64")]
const INTERRUPT_MODEL: uacpi_sys::uacpi_interrupt_model = uacpi_sys::UACPI_INTERRUPT_MODEL_RINTC;

pub fn set_rsdp(addr: u64) {
    RSDP.store(addr, Ordering::Relaxed);
}

const LOG_LEVELS: &[(&str, uacpi_sys::uacpi_log_level)] = &[
    ("error", uacpi_sys::UACPI_LOG_ERROR),
    ("warn", uacpi_sys::UACPI_LOG_WARN),
    ("info", uacpi_sys::UACPI_LOG_INFO),
    ("trace", uacpi_sys::UACPI_LOG_TRACE),
    ("debug", uacpi_sys::UACPI_LOG_DEBUG),
];

/// Applies the `uacpi.log` kernel command line option.
pub fn configure_log_level(cmdline: &str) {
    let Some(name) = cmdline
        .split_ascii_whitespace()
        .find_map(|opt| opt.strip_prefix("uacpi.log="))
    else {
        return;
    };

    let Some(&(_, level)) = LOG_LEVELS.iter().find(|(candidate, _)| *candidate == name) else {
        println!("sif: ignoring unknown uacpi.log level \"{name}\"");
        return;
    };

    unsafe { uacpi_sys::uacpi_context_set_log_level(level) };
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
            uacpi_sys::uacpi_set_interrupt_model(INTERRUPT_MODEL),
            "uacpi_set_interrupt_model",
        )?;
        check(
            uacpi_sys::uacpi_namespace_initialize(),
            "uacpi_namespace_initialize",
        )?;
    }

    Ok(())
}
