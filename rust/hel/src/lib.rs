//! A Rust wrapper for Hel.
#![allow(incomplete_features)]
#![feature(generic_const_exprs)]
#![feature(local_waker)]

pub mod executor;
pub mod handle;
pub mod mapping;
pub mod queue;
pub mod result;
pub mod submission;

use std::time::Duration;

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
pub use submission::{sleep_for, sleep_until, submit_async};

/// Creates a pair of connected lanes that can be used to communicate.
pub fn create_stream() -> Result<(Handle, Handle)> {
    let mut lane1 = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    let mut lane2 = hel_sys::kHelNullHandle as hel_sys::HelHandle;

    result::hel_check(unsafe { hel_sys::helCreateStream(&mut lane1, &mut lane2, 0) })?;

    // SAFETY: helCreateStream returns two freshly created handles in the current universe.
    Ok(unsafe { (Handle::from_raw(lane1), Handle::from_raw(lane2)) })
}

/// Creates a new, empty address space that threads can run in.
pub fn create_space() -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe { hel_sys::helCreateSpace(&mut handle) })?;
    Ok(unsafe { Handle::from_raw(handle) })
}

/// Creates a new DMA space that memory can be mapped into for device DMA.
pub fn create_dma_space() -> Result<Handle> {
    let mut handle = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    result::hel_check(unsafe { hel_sys::helCreateDmaSpace(0, &mut handle) })?;
    Ok(unsafe { Handle::from_raw(handle) })
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
pub fn enable_io(handle: Handle) -> Result<()> {
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
            std::ptr::null(),
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
            std::ptr::null(),
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
    let mut buffer = [0 as std::ffi::c_char; hel_sys::kHelIrqNameSize as usize];
    for (slot, &byte) in buffer.iter_mut().zip(name.as_bytes()) {
        *slot = byte as std::ffi::c_char;
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

impl std::ops::Add<Duration> for Time {
    type Output = Self;

    fn add(self, rhs: Duration) -> Self::Output {
        Self(self.0 + rhs.as_nanos() as u64)
    }
}

impl std::ops::Sub<Duration> for Time {
    type Output = Self;

    fn sub(self, rhs: Duration) -> Self::Output {
        Self(self.0 - rhs.as_nanos() as u64)
    }
}
