use thiserror::Error;

/// Reasons why sif cannot use a device tree node or one of its properties.
#[derive(Debug, Error)]
pub enum DtError {
    #[error("#interrupt-cells is {cells}, which the interrupt controller does not support")]
    UnsupportedInterruptCells { cells: usize },

    #[error("GIC interrupt type {type_} is not supported")]
    UnsupportedGicInterruptType { type_: u64 },

    #[error("invalid IRQ flags {flags:#x}")]
    InvalidIrqFlags { flags: u64 },
}

impl From<DtError> for managarm::hw::Error {
    fn from(_: DtError) -> Self {
        managarm::hw::Error::DeviceError
    }
}
