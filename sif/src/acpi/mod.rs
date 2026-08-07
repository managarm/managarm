pub mod glue;
pub mod io;

use std::collections::BTreeMap;
use std::ptr::addr_of;
use std::sync::atomic::{AtomicU64, Ordering};

use anyhow::{Result, anyhow};
use parking_lot::Mutex;

use uacpi_sys::uacpi_status;

pub(crate) const PAGE_SIZE: usize = 0x1000;
pub(crate) const PAGE_MASK: usize = PAGE_SIZE - 1;

pub(crate) static RSDP: AtomicU64 = AtomicU64::new(0);

pub fn set_rsdp(addr: u64) {
    RSDP.store(addr, Ordering::Relaxed);
}

#[derive(Clone, Copy)]
struct EcamRegion {
    address: u64,
    segment: u16,
    start_bus: u8,
    end_bus: u8,
}

struct EcamState {
    regions: Vec<EcamRegion>,
    windows: BTreeMap<u64, hel::Mapping<u8>>,
}

impl EcamState {
    const fn new() -> Self {
        Self {
            regions: Vec::new(),
            windows: BTreeMap::new(),
        }
    }
}

unsafe impl Send for EcamState {}

static ECAM: Mutex<EcamState> = Mutex::new(EcamState::new());

fn legacy_address(bus: u8, slot: u8, func: u8, offset: u16) -> u32 {
    0x8000_0000
        | ((bus as u32) << 16)
        | ((slot as u32) << 11)
        | ((func as u32) << 8)
        | (offset as u32 & 0xFC)
}

fn ecam_config_ptr(segment: u16, bus: u8, slot: u8, func: u8) -> Option<*mut u8> {
    let mut state = ECAM.lock();

    let region = *state
        .regions
        .iter()
        .find(|r| r.segment == segment && bus >= r.start_bus && bus <= r.end_bus)?;

    let phys = region.address
        + (((bus - region.start_bus) as u64) << 20)
        + ((slot as u64) << 15)
        + ((func as u64) << 12);

    if !state.windows.contains_key(&phys) {
        let handle = hel::access_physical(phys as usize, PAGE_SIZE).ok()?;
        let mapping = unsafe {
            hel::Mapping::<u8>::new(
                &handle,
                None,
                0,
                PAGE_SIZE,
                hel::MappingFlags::READ | hel::MappingFlags::WRITE,
            )
        }
        .ok()?;
        state.windows.insert(phys, mapping);
    }

    let mapping = state.windows.get(&phys)?;
    Some(unsafe { mapping.as_ptr() }?.as_ptr())
}

unsafe fn mmio_read(base: *mut u8, offset: u16, size: u8) -> u32 {
    let ptr = unsafe { base.add(offset as usize) };
    match size {
        1 => unsafe { core::ptr::read_volatile(ptr) as u32 },
        2 => unsafe { core::ptr::read_volatile(ptr as *const u16) as u32 },
        4 => unsafe { core::ptr::read_volatile(ptr as *const u32) },
        _ => 0xFFFFFFFF,
    }
}

unsafe fn mmio_write(base: *mut u8, offset: u16, size: u8, value: u32) {
    let ptr = unsafe { base.add(offset as usize) };
    match size {
        1 => unsafe { core::ptr::write_volatile(ptr, value as u8) },
        2 => unsafe { core::ptr::write_volatile(ptr as *mut u16, value as u16) },
        4 => unsafe { core::ptr::write_volatile(ptr as *mut u32, value) },
        _ => {}
    }
}

pub fn config_read(segment: u16, bus: u8, slot: u8, func: u8, offset: u16, size: u8) -> u32 {
    if let Some(base) = ecam_config_ptr(segment, bus, slot, func) {
        return unsafe { mmio_read(base, offset, size) };
    }

    if segment != 0 {
        return 0xFFFFFFFF;
    }
    unsafe {
        io::outl(0xCF8, legacy_address(bus, slot, func, offset));
        let word = io::inl(0xCFC);
        match size {
            1 => (word >> ((offset & 3) * 8)) & 0xFF,
            2 => (word >> ((offset & 2) * 8)) & 0xFFFF,
            4 => word,
            _ => 0xFFFFFFFF,
        }
    }
}

pub fn config_write(segment: u16, bus: u8, slot: u8, func: u8, offset: u16, size: u8, value: u32) {
    if let Some(base) = ecam_config_ptr(segment, bus, slot, func) {
        unsafe { mmio_write(base, offset, size, value) };
        return;
    }

    if segment != 0 {
        return;
    }
    unsafe {
        io::outl(0xCF8, legacy_address(bus, slot, func, offset));
        if size == 4 {
            io::outl(0xCFC, value);
        } else {
            let word = io::inl(0xCFC);
            let shift = (offset & 3) * 8;
            let mask = if size == 1 { 0xFFu32 } else { 0xFFFFu32 } << shift;
            io::outl(0xCFC, (word & !mask) | ((value << shift) & mask));
        }
    }
}

fn load_mcfg() {
    let mut table = uacpi_sys::uacpi_table::default();
    let status = unsafe { uacpi_sys::uacpi_table_find_by_signature(c"MCFG".as_ptr(), &mut table) };
    if status != uacpi_sys::UACPI_STATUS_OK {
        return;
    }

    let hdr = unsafe { table.__bindgen_anon_1.hdr };
    if hdr.is_null() {
        unsafe { uacpi_sys::uacpi_table_unref(&mut table) };
        return;
    }
    let mcfg = hdr as *const uacpi_sys::acpi_mcfg;

    let length = unsafe { addr_of!((*mcfg).hdr.length).read_unaligned() } as usize;
    let header_size = size_of::<uacpi_sys::acpi_mcfg>();
    let entry_size = size_of::<uacpi_sys::acpi_mcfg_allocation>();
    let count = length.saturating_sub(header_size) / entry_size;

    let entries =
        unsafe { (mcfg as *const u8).add(header_size) as *const uacpi_sys::acpi_mcfg_allocation };
    let mut regions = Vec::with_capacity(count);
    for i in 0..count {
        let entry = unsafe { entries.add(i) };
        regions.push(EcamRegion {
            address: unsafe { addr_of!((*entry).address).read_unaligned() },
            segment: unsafe { addr_of!((*entry).segment).read_unaligned() },
            start_bus: unsafe { addr_of!((*entry).start_bus).read_unaligned() },
            end_bus: unsafe { addr_of!((*entry).end_bus).read_unaligned() },
        });
    }

    unsafe { uacpi_sys::uacpi_table_unref(&mut table) };

    if !regions.is_empty() {
        eprintln!("sif: MCFG describes {} ECAM region(s)", regions.len());
        ECAM.lock().regions = regions;
    }
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
        load_mcfg();
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
