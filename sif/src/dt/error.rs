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

    #[error("the node has interrupts, but no interrupt parent")]
    NoInterruptParent,

    #[error("the interrupt parent {parent} has no #interrupt-cells")]
    NoInterruptCells { parent: String },

    #[error("the node has no {name} property")]
    MissingProperty { name: &'static str },

    #[error("the {name} property is truncated")]
    TruncatedProperty { name: &'static str },

    #[error("no node has the phandle {phandle}")]
    DanglingPhandle { phandle: u32 },
}

impl From<DtError> for managarm::hw::Error {
    fn from(_: DtError) -> Self {
        managarm::hw::Error::DeviceError
    }
}
