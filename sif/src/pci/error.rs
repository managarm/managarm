use thiserror::Error;

/// Reasons why sif cannot use a PCI function or one of its features.
#[derive(Debug, Error)]
pub enum PciError {
    #[error("BAR #{index} has the reserved type 3")]
    ReservedBarType { index: usize },

    #[error("64-bit BAR #{index} has no upper half")]
    TruncatedBar64 { index: usize },

    #[error("the capability list does not terminate")]
    CapabilityLoop,

    #[error("the extended capability list does not terminate")]
    ExtendedCapabilityLoop,

    #[error("the bridge is configured, but sits below an unconfigured bridge")]
    ConfiguredBridgeBelowUnconfigured,

    #[error("BAR #{bir}, which holds the MSI-X table, is not allocated")]
    MsixTableBarUnallocated { bir: usize },

    #[error("the MSI-X table does not fit into BAR #{bir}")]
    MsixTableOutsideBar { bir: usize },

    #[error("the MSI-X table is in BIR {bir}, which is not a memory BAR")]
    MsixTableNotInMemoryBar { bir: usize },

    #[error(transparent)]
    Hel(#[from] hel::Error),
}
