//! A Rust wrapper for Hel.
#![no_std]
#![allow(incomplete_features)]
#![feature(generic_const_exprs)]

extern crate alloc;
#[cfg(feature = "std")]
extern crate std;

pub mod executor;
pub mod handle;
pub mod mapping;
pub mod queue;
pub mod result;
pub mod submission;

use alloc::vec::Vec;
use core::time::Duration;

#[cfg(feature = "std")]
pub use executor::{block_on, spawn};
pub use handle::Handle;
pub use mapping::{Mapping, MappingFlags};
pub use queue::Queue;
pub use result::{Error, Result};
pub use submission::action::{
    Accept, Dismiss, ExtractCredentials, Offer, PullDescriptor, PushDescriptor, ReceiveBuffer,
    ReceiveInline, SendBuffer,
};
#[cfg(feature = "std")]
pub use submission::{await_event, sleep_for, sleep_until, submit_async};

/// Creates a pair of connected lanes that can be used to communicate.
pub fn create_stream() -> Result<(Handle, Handle)> {
    let mut lane1 = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    let mut lane2 = hel_sys::kHelNullHandle as hel_sys::HelHandle;

    result::hel_check(unsafe { hel_sys::helCreateStream(&mut lane1, &mut lane2, 0) })?;

    // SAFETY: helCreateStream returns two freshly created handles in the current universe.
    Ok(unsafe { (Handle::from_raw(lane1), Handle::from_raw(lane2)) })
}

/// Creates an event that counts how often it was raised.
/// Returns the wait handle and the raise handle (in this order).
/// Each side gets [`Error::EndOfLane`] once all handles to the other end are closed.
pub fn create_sequenced_event() -> Result<(Handle, Handle)> {
    let mut wait_handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    let mut raise_handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;

    result::hel_check(unsafe {
        hel_sys::helCreateSequencedEvent(&mut wait_handle, &mut raise_handle)
    })?;

    // SAFETY: helCreateSequencedEvent returns two freshly created handles in the current universe.
    Ok(unsafe {
        (
            Handle::from_raw(wait_handle),
            Handle::from_raw(raise_handle),
        )
    })
}

/// Raises a oneshot event or the raise end of a sequenced event.
pub fn raise_event(event: &Handle) -> Result<()> {
    result::hel_check(unsafe { hel_sys::helRaiseEvent(event.handle()) })
}

/// Creates a new, empty address space that threads can run in.
pub fn create_space() -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe { hel_sys::helCreateSpace(&mut handle) })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Creates a slice that covers `size` bytes of a memory object, starting at `offset`.
///
/// Both `offset` and `size` must be aligned to the page size. The memory object handle
/// requires [`kHelRightAssign`](hel_sys::kHelRightAssign). The slice inherits its read,
/// write, execute and assign rights.
pub fn create_slice_view(bundle: &Handle, offset: usize, size: usize) -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe {
        hel_sys::helCreateSliceView(bundle.handle(), offset, size, 0, &mut handle)
    })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Creates a child of the given hierarchy node. The tag identifies it in kernel messages.
///
/// With a `memory_floor`, the child gets its own memory controller: allocations of the child
/// and its descendants wait (or fail) unless that many bytes of memory stay free.
pub fn extend_hierarchy(parent: &Handle, tag: &str, memory_floor: Option<usize>) -> Result<Handle> {
    let mut params = hel_sys::HelHierarchyParameters {
        tag: [0; 128],
        controllers: 0,
        memoryFloor: 0,
    };
    // Tags that do not fit are truncated.
    for (slot, &byte) in params.tag.iter_mut().zip(tag.as_bytes()) {
        *slot = byte as core::ffi::c_char;
    }
    if let Some(floor) = memory_floor {
        params.controllers |= hel_sys::kHelHierarchyMemoryController;
        params.memoryFloor = floor;
    }

    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe {
        hel_sys::helExtendHierarchy(parent.handle(), &params, &mut handle)
    })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// A swap space, the backing store for swappable anonymous memory.
///
/// The swap daemon that owns it services manage requests on the
/// [backing handle](SwapSpace::backing_handle), while the
/// [swap handle](SwapSpace::swap_handle) is handed out to whoever allocates
/// swappable memory.
pub struct SwapSpace {
    backing_handle: Handle,
    swap_handle: Handle,
}

impl SwapSpace {
    /// Returns the handle to the swap space's memory object.
    ///
    /// It is addressed by kernel-allocated swap offsets. The swap daemon maps it to
    /// perform disk I/O and receives initialize and writeback requests on it through
    /// [`submission::manage_memory`].
    pub fn backing_handle(&self) -> &Handle {
        &self.backing_handle
    }

    /// Returns the handle that identifies the swap space when allocating
    /// swappable memory.
    pub fn swap_handle(&self) -> &Handle {
        &self.swap_handle
    }
}

/// Creates a new swap space.
///
/// The resident frames of all memory backed by the swap space are accounted to
/// the given hierarchy node.
pub fn create_swap_space(hierarchy: &Handle) -> Result<SwapSpace> {
    let mut backing_handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    let mut swap_handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe {
        hel_sys::helCreateSwapSpace(hierarchy.handle(), 0, &mut backing_handle, &mut swap_handle)
    })?;
    Ok(SwapSpace {
        backing_handle: unsafe { Handle::from_raw(backing_handle) },
        swap_handle: unsafe { Handle::from_raw(swap_handle) },
    })
}

/// Completes a manage request that was obtained through [`submission::manage_memory`].
///
/// For swap spaces, all locks and mappings of the requested range must be dropped
/// before calling this; otherwise, it fails with [`Error::IllegalState`].
pub fn complete_manage_request(
    memory: &Handle,
    request: submission::result::ManageRequest,
) -> Result<()> {
    let type_ = match request {
        submission::result::ManageRequest::Initialize { .. } => hel_sys::kHelManageInitialize,
        submission::result::ManageRequest::Writeback { .. } => hel_sys::kHelManageWriteback,
    };
    result::hel_check(unsafe {
        hel_sys::helUpdateMemory(
            memory.handle(),
            type_ as _,
            request.offset(),
            request.length(),
        )
    })
}

/// The firmware description that an IOMMU was discovered from.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IommuKind {
    IntelVtd,
    AmdVi,
}

impl IommuKind {
    fn to_raw(self) -> u32 {
        match self {
            Self::IntelVtd => hel_sys::kHelAccessIommuIntelVtd,
            Self::AmdVi => hel_sys::kHelAccessIommuAmdVi,
        }
    }
}

/// A range of physical memory that a DMA space identity-maps.
#[derive(Debug, Clone, Copy)]
pub struct DmaReservedRegion {
    pub base: u64,
    pub size: u64,
    pub readable: bool,
    pub writable: bool,
}

/// The requester ID that a device issues DMA requests as.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct DmaDeviceId {
    pub segment: u16,
    pub bus: u8,
    pub slot: u8,
    pub function: u8,
}

impl DmaDeviceId {
    fn to_raw(self) -> hel_sys::HelDmaDeviceId {
        hel_sys::HelDmaDeviceId {
            segment: self.segment.into(),
            bus: self.bus,
            slot: self.slot,
            function: self.function,
        }
    }
}

/// Returns the IOMMU that the kernel discovered at the given register base.
pub fn access_iommu(access_handle: &Handle, kind: IommuKind, base: u64) -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe {
        hel_sys::helAccessIommu(access_handle.handle(), kind.to_raw(), base, &mut handle)
    })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Creates a DMA space that memory can be mapped into for device DMA.
///
/// With an IOMMU, the space is one IOMMU domain that the unit translates; the reserved
/// regions are identity-mapped and taken out of the range that the space allocates addresses
/// from. `None` creates a space that does not translate and takes no reserved regions.
pub fn create_dma_space(iommu: Option<&Handle>, regions: &[DmaReservedRegion]) -> Result<Handle> {
    let regions: Vec<hel_sys::HelDmaReservedRegion> = regions
        .iter()
        .map(|region| {
            let mut flags = 0;
            if region.readable {
                flags |= hel_sys::kHelDmaRegionRead;
            }
            if region.writable {
                flags |= hel_sys::kHelDmaRegionWrite;
            }
            hel_sys::HelDmaReservedRegion {
                base: region.base,
                size: region.size,
                flags,
            }
        })
        .collect();

    let iommu = iommu.map_or(hel_sys::kHelNullHandle as hel_sys::HelHandle, |i| {
        i.handle()
    });
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe {
        hel_sys::helCreateDmaSpace(iommu, regions.as_ptr(), regions.len(), 0, &mut handle)
    })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

bitflags::bitflags! {
    /// Flags that control how a memory object is allocated.
    #[derive(Debug, Clone, Copy, PartialEq, Eq)]
    pub struct AllocFlags: u32 {
        /// The memory is physically contiguous.
        const CONTINUOUS = hel_sys::kHelAllocContinuous;
        /// Physical pages are only allocated when they are first touched.
        const ON_DEMAND = hel_sys::kHelAllocOnDemand;
    }
}

/// Allocates a memory object of `size` bytes that is accounted to `hierarchy`, optionally
/// restricted to physical addresses that fit into `address_bits` (e.g. for devices with a
/// limited DMA mask).
pub fn allocate_memory(
    hierarchy: &Handle,
    size: usize,
    flags: AllocFlags,
    address_bits: Option<u32>,
) -> Result<Handle> {
    let restrictions = hel_sys::HelAllocRestrictions {
        addressBits: address_bits.map_or(Ok(64), |bits| {
            i32::try_from(bits).map_err(|_| Error::IllegalArgs)
        })?,
    };
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe {
        hel_sys::helAllocateMemory(
            hierarchy.handle(),
            size,
            flags.bits(),
            &restrictions,
            &mut handle,
        )
    })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Translates a virtual address of the current address space to the physical address
/// that backs it. The page must be mapped and populated.
pub fn pointer_physical(pointer: *const core::ffi::c_void) -> Result<usize> {
    let mut physical = 0;
    result::hel_check(unsafe {
        hel_sys::helPointerPhysical(
            hel_sys::kHelNullHandle as hel_sys::HelHandle,
            pointer,
            &mut physical,
        )
    })?;
    Ok(physical)
}

/// The caching mode that a memory object is mapped with.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum CachingMode {
    /// Let the kernel pick a caching mode.
    Default,
    Uncached,
    WriteCombine,
    WriteThrough,
    WriteBack,
    Mmio,
    MmioNonPosted,
}

impl CachingMode {
    fn to_raw(self) -> u32 {
        match self {
            Self::Default => hel_sys::kHelCachingDefault,
            Self::Uncached => hel_sys::kHelCachingUncached,
            Self::WriteCombine => hel_sys::kHelCachingWriteCombine,
            Self::WriteThrough => hel_sys::kHelCachingWriteThrough,
            Self::WriteBack => hel_sys::kHelCachingWriteBack,
            Self::Mmio => hel_sys::kHelCachingMmio,
            Self::MmioNonPosted => hel_sys::kHelCachingMmioNonPosted,
        }
    }
}

/// Returns a memory object backing the given physical memory range.
pub fn access_physical(
    access_handle: &Handle,
    physical: usize,
    size: usize,
    caching: CachingMode,
) -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe {
        hel_sys::helAccessPhysical(
            access_handle.handle(),
            physical,
            size,
            caching.to_raw(),
            &mut handle,
        )
    })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Enables IO access on the given IO port handle.
pub fn enable_io(handle: &Handle) -> Result<()> {
    result::hel_check(unsafe { hel_sys::helEnableIo(handle.handle()) })
}

/// Returns an IO-space object granting access to the given set of IO ports.
pub fn access_io(access_handle: &Handle, ports: &[usize]) -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe {
        hel_sys::helAccessIo(
            access_handle.handle(),
            ports.as_ptr(),
            ports.len(),
            &mut handle,
        )
    })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Returns the IRQ pin that the given global system interrupt is attached to.
pub fn access_irq_by_gsi(access_handle: &Handle, gsi: u64) -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe {
        hel_sys::helAccessIrq(
            access_handle.handle(),
            hel_sys::kHelAccessIrqByGsi as u32,
            0,
            gsi,
            core::ptr::null(),
            &mut handle,
        )
    })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Returns the IRQ pin identified by a device tree phandle and a controller-specific index.
pub fn access_irq_by_phandle(access_handle: &Handle, phandle: u64, index: u64) -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe {
        hel_sys::helAccessIrq(
            access_handle.handle(),
            hel_sys::kHelAccessIrqByPhandle as u32,
            phandle,
            index,
            core::ptr::null(),
            &mut handle,
        )
    })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Allocates a fresh MSI pin from the system's MSI controller.
/// The name identifies the pin in kernel diagnostics.
pub fn allocate_msi(access_handle: &Handle, name: &str) -> Result<Handle> {
    // The kernel reads the whole buffer, hence pass one of exactly that size.
    // Names that do not fit are truncated.
    let mut buffer = [0 as core::ffi::c_char; hel_sys::kHelIrqNameSize as usize];
    for (slot, &byte) in buffer.iter_mut().zip(name.as_bytes()) {
        *slot = byte as core::ffi::c_char;
    }

    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe {
        hel_sys::helAccessIrq(
            access_handle.handle(),
            hel_sys::kHelAccessIrqAllocateMsi as u32,
            0,
            0,
            buffer.as_ptr(),
            &mut handle,
        )
    })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Message address / data pair that raises an MSI.
#[derive(Debug, Clone, Copy)]
pub struct MsiInfo {
    pub address: u64,
    pub data: u32,
}

/// Queries the message address and data of an MSI pin.
pub fn query_msi_info(pin: &Handle) -> Result<MsiInfo> {
    let mut info = hel_sys::HelMsiInfo {
        address: 0,
        data: 0,
    };
    result::hel_check(unsafe { hel_sys::helQueryMsiInfo(pin.handle(), &mut info) })?;
    Ok(MsiInfo {
        address: info.address,
        data: info.data,
    })
}

/// Returns an IRQ object that handles interrupts of the given IRQ pin.
pub fn handle_irq(pin: &Handle) -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe { hel_sys::helHandleIrq(pin.handle(), &mut handle) })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Acknowledges or rejects the IRQ that was raised for a sequence number.
pub fn acknowledge_irq(handle: &Handle, flags: u32, sequence: u64) -> Result<()> {
    result::hel_check(unsafe { hel_sys::helAcknowledgeIrq(handle.handle(), flags, sequence) })
}

/// The trigger mode of an interrupt.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IrqTrigger {
    Edge,
    Level,
}

/// The polarity of an interrupt.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum IrqPolarity {
    High,
    Low,
}

/// Configures the trigger mode and polarity of an IRQ pin.
///
/// `None` states that the interrupt controller has no configurable trigger mode / polarity.
pub fn configure_irq(
    pin: &Handle,
    trigger: Option<IrqTrigger>,
    polarity: Option<IrqPolarity>,
) -> Result<()> {
    let trigger = match trigger {
        None => hel_sys::kHelIrqTriggerNull,
        Some(IrqTrigger::Edge) => hel_sys::kHelIrqTriggerEdge,
        Some(IrqTrigger::Level) => hel_sys::kHelIrqTriggerLevel,
    };
    let polarity = match polarity {
        None => hel_sys::kHelIrqPolarityNull,
        Some(IrqPolarity::High) => hel_sys::kHelIrqPolarityHigh,
        Some(IrqPolarity::Low) => hel_sys::kHelIrqPolarityLow,
    };
    result::hel_check(unsafe {
        hel_sys::helConfigureIrq(pin.handle(), trigger as u32, polarity as u32)
    })
}

/// A time value in nanoseconds since boot.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Time(u64);

impl Time {
    /// Creates a new [`Time`] instance representing the current time
    /// since boot in nanoseconds.
    pub fn new_since_boot() -> Result<Self> {
        let mut nanos = 0;

        result::hel_check(unsafe { hel_sys::helGetClock(&mut nanos) }).map(|_| Self(nanos))
    }

    /// Creates a new [`Time`] instance from the given number of
    /// nanoseconds since boot.
    pub fn from_nanos(nanos: u64) -> Self {
        Self(nanos)
    }

    /// Returns the value of the clock in nanoseconds since boot.
    pub fn nanos(&self) -> u64 {
        self.0
    }
}

impl core::ops::Add<Duration> for Time {
    type Output = Self;

    fn add(self, rhs: Duration) -> Self::Output {
        Self(self.0 + rhs.as_nanos() as u64)
    }
}

impl core::ops::Sub<Duration> for Time {
    type Output = Self;

    fn sub(self, rhs: Duration) -> Self::Output {
        Self(self.0 - rhs.as_nanos() as u64)
    }
}
