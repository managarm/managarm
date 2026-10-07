pub mod ecam;
pub mod legacy;

use std::collections::BTreeMap;
use std::sync::Mutex;

use anyhow::Context;
use thiserror::Error;

use ecam::EcamPcieConfigIo;
use legacy::LegacyPciConfigIo;

use crate::uacpi::table::Table;

/// Reasons why an access to PCI configuration space can fail.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Error)]
pub enum ConfigIoError {
    #[error("there is no configuration space for {seg:04x}:{bus:02x}")]
    NoConfigSpace { seg: u16, bus: u8 },

    #[error("{seg:04x}:{bus:02x}:{slot:02x}.{function} is not addressable by this backend")]
    InvalidAddress {
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
    },

    #[error("register {offset:#x} lies outside of the {limit:#x} byte configuration space")]
    OutOfRange { offset: u16, limit: u16 },

    #[error("register {offset:#x} is not aligned to {size} bytes")]
    Misaligned { offset: u16, size: u8 },

    #[error("failed to map the ECAM window of {seg:04x}:{bus:02x}")]
    MappingFailed {
        seg: u16,
        bus: u8,
        #[source]
        source: hel::Error,
    },
}

pub type Result<T> = std::result::Result<T, ConfigIoError>;

/// Checks that a register of `size` bytes at `offset` fits into a `limit` byte config space.
pub(crate) fn check_offset(offset: u16, size: u8, limit: u16) -> Result<()> {
    if offset % size as u16 != 0 {
        return Err(ConfigIoError::Misaligned { offset, size });
    }
    if offset as usize + size as usize > limit as usize {
        return Err(ConfigIoError::OutOfRange { offset, limit });
    }
    Ok(())
}

/// Raw access to the configuration space of a (segment, bus) pair.
///
/// Since the effect of an access depends on the register that is addressed, all accessors are
/// unsafe; safe accessors for individual registers are built on top of them.
pub trait PciConfigIo: Sync {
    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn read_config_byte(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u8>;

    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn read_config_half(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u16>;

    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn read_config_word(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u32>;

    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn write_config_byte(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u8,
    ) -> Result<()>;

    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn write_config_half(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u16,
    ) -> Result<()>;

    /// # Safety
    ///
    /// See [`PciConfigIo`].
    unsafe fn write_config_word(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u32,
    ) -> Result<()>;

    fn supports_4k_config_space(&self) -> bool;
}

static CONFIG_SPACES: Mutex<BTreeMap<u32, &'static dyn PciConfigIo>> = Mutex::new(BTreeMap::new());

pub fn add_config_space_io(seg: u16, bus: u8, io: &'static dyn PciConfigIo) {
    CONFIG_SPACES
        .lock()
        .expect("sif: config space registry mutex was poisoned")
        .insert(((seg as u32) << 8) | bus as u32, io);
}

pub fn get_config_io_for(seg: u16, bus: u8) -> Option<&'static dyn PciConfigIo> {
    CONFIG_SPACES
        .lock()
        .expect("sif: config space registry mutex was poisoned")
        .get(&(((seg as u32) << 8) | bus as u32))
        .copied()
}

fn config_io_for(seg: u16, bus: u8) -> Result<&'static dyn PciConfigIo> {
    get_config_io_for(seg, bus).ok_or(ConfigIoError::NoConfigSpace { seg, bus })
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn read_config_byte(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
) -> Result<u8> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.read_config_byte(seg, bus, slot, function, offset) }
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn read_config_half(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
) -> Result<u16> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.read_config_half(seg, bus, slot, function, offset) }
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn read_config_word(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
) -> Result<u32> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.read_config_word(seg, bus, slot, function, offset) }
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn write_config_byte(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
    value: u8,
) -> Result<()> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.write_config_byte(seg, bus, slot, function, offset, value) }
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn write_config_half(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
    value: u16,
) -> Result<()> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.write_config_half(seg, bus, slot, function, offset, value) }
}

/// # Safety
///
/// See [`PciConfigIo`].
pub unsafe fn write_config_word(
    seg: u16,
    bus: u8,
    slot: u8,
    function: u8,
    offset: u16,
    value: u32,
) -> Result<()> {
    let io = config_io_for(seg, bus)?;
    unsafe { io.write_config_word(seg, bus, slot, function, offset, value) }
}

fn add_legacy_config_io() -> anyhow::Result<()> {
    // Unlike thor, we need to be granted access to the config window ports.
    let io =
        LegacyPciConfigIo::new().context("failed to access the legacy PCI config I/O ports")?;
    let io: &'static LegacyPciConfigIo = Box::leak(Box::new(io));
    for bus in 0..=255u8 {
        add_config_space_io(0, bus, io);
    }
    Ok(())
}

pub fn discover_config_spaces() -> anyhow::Result<()> {
    let table = match Table::find_by_signature(c"MCFG") {
        Ok(Some(table)) => table,
        Ok(None) | Err(_) => {
            println!("sif: No MCFG table, assuming legacy PCI");
            return add_legacy_config_io();
        }
    };

    struct EcamRegion {
        address: u64,
        segment: u16,
        start_bus: u8,
        end_bus: u8,
    }
    let entries = table
        .bytes()
        .get(size_of::<uacpi_sys::acpi_mcfg>()..)
        .unwrap_or_default();
    let mut regions = Vec::new();
    for entry in entries.chunks_exact(size_of::<uacpi_sys::acpi_mcfg_allocation>()) {
        // SAFETY: every byte pattern of the size of a packed structure is a valid value.
        let entry: uacpi_sys::acpi_mcfg_allocation =
            unsafe { std::ptr::read_unaligned(entry.as_ptr().cast()) };
        regions.push(EcamRegion {
            address: entry.address,
            segment: entry.segment,
            start_bus: entry.start_bus,
            end_bus: entry.end_bus,
        });
    }
    drop(table);

    if regions.is_empty() {
        println!("sif: MCFG table has no entries, assuming legacy PCI");
        return add_legacy_config_io();
    }

    for region in regions {
        println!(
            "sif: Found config space for segment {}, buses {}-{}, ECAM MMIO base at {:#x}",
            region.segment, region.start_bus, region.end_bus, region.address
        );

        // MCFG base addresses are relative to bus 0, while EcamPcieConfigIo expects
        // a window that starts at the first bus that it decodes.
        let io: &'static EcamPcieConfigIo = Box::leak(Box::new(EcamPcieConfigIo::new(
            region.address + ((region.start_bus as u64) << 20),
            region.segment,
            region.start_bus,
            region.end_bus,
        )));
        for bus in region.start_bus..=region.end_bus {
            add_config_space_io(region.segment, bus, io);
        }
    }

    Ok(())
}
