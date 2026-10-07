use uacpi_sys::{uacpi_host_interface, uacpi_interrupt_model, uacpi_log_level};

use super::runtime::Aml;
use super::{Result, check};

/// Wraps uacpi_context_set_log_level().
pub fn context_set_log_level(level: uacpi_log_level) {
    // SAFETY: uACPI does not take any arguments that we could get wrong.
    unsafe { uacpi_sys::uacpi_context_set_log_level(level) };
}

/// Wraps uacpi_initialize().
pub fn initialize() -> Result<()> {
    // SAFETY: uACPI does not take any arguments that we could get wrong.
    check("uacpi_initialize", unsafe {
        uacpi_sys::uacpi_initialize(0)
    })
}

/// Wraps uacpi_enable_host_interface().
pub fn enable_host_interface(interface: uacpi_host_interface) -> Result<()> {
    // SAFETY: uACPI does not take any arguments that we could get wrong.
    check("uacpi_enable_host_interface", unsafe {
        uacpi_sys::uacpi_enable_host_interface(interface)
    })
}

/// Wraps uacpi_namespace_load(), i.e., runs the table-level AML of the DSDT and SSDTs.
pub fn namespace_load(_aml: Aml) -> Result<()> {
    // SAFETY: uACPI does not take any arguments that we could get wrong.
    check("uacpi_namespace_load", unsafe {
        uacpi_sys::uacpi_namespace_load()
    })
}

/// Wraps uacpi_set_interrupt_model(), i.e., evaluates _PIC.
pub fn set_interrupt_model(_aml: Aml, model: uacpi_interrupt_model) -> Result<()> {
    // SAFETY: uACPI does not take any arguments that we could get wrong.
    check("uacpi_set_interrupt_model", unsafe {
        uacpi_sys::uacpi_set_interrupt_model(model)
    })
}

/// Wraps uacpi_namespace_initialize(), i.e., evaluates _STA and _INI namespace-wide.
pub fn namespace_initialize(_aml: Aml) -> Result<()> {
    // SAFETY: uACPI does not take any arguments that we could get wrong.
    check("uacpi_namespace_initialize", unsafe {
        uacpi_sys::uacpi_namespace_initialize()
    })
}
