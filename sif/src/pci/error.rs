use thiserror::Error;

use crate::dt::DtError;
use crate::uacpi;

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

    #[error("MSI address {address:#x} does not fit into the 32-bit MSI capability")]
    MsiAddressTooWide { address: u64 },

    #[error(
        "failed to bind requester {:04x}:{:02x}:{:02x}.{}",
        id.segment, id.bus, id.slot, id.function
    )]
    IommuBind {
        id: hel::DmaDeviceId,
        #[source]
        source: hel::Error,
    },

    #[error("the host bridge is not supported")]
    UnsupportedHostBridge,

    #[error("the ECAM host bridge has {count} reg entries instead of one")]
    EcamRegCount { count: usize },

    #[error("a range of the host bridge has no PCI address")]
    RangeWithoutPciAddress,

    #[error("the route is for bus {bus:02x}")]
    RouteForOtherBus { bus: u32 },

    #[error("routes of individual functions are not supported")]
    FunctionRoute,

    #[error("invalid interrupt pin {pin}")]
    InvalidPin { pin: u64 },

    #[error("failed to evaluate the _PRT")]
    PrtEvaluation(#[source] uacpi::Error),

    #[error("failed to evaluate the _CRS of the IRQ link")]
    LinkResources(#[source] uacpi::Error),

    #[error("the _CRS of the IRQ link has no resource {index}")]
    LinkResourceMissing { index: u32 },

    #[error("resource {index} of the IRQ link does not describe an IRQ")]
    LinkResourceNotIrq { index: u32 },

    #[error("the IRQ link is not connected to any IRQ")]
    LinkNotConnected,

    #[error("failed to set up IRQ {index} of {controller}")]
    DtIrqSetup {
        index: u64,
        controller: String,
        #[source]
        source: hel::Error,
    },

    #[error(transparent)]
    Dt(#[from] DtError),

    #[error(transparent)]
    Hel(#[from] hel::Error),
}

impl From<PciError> for managarm::hw::Error {
    fn from(err: PciError) -> Self {
        match err {
            PciError::Hel(err) => managarm::hw::Error::HelError(err),
            _ => managarm::hw::Error::DeviceError,
        }
    }
}
