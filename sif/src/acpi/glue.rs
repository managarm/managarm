use arch::{PioAccess, PioSpace};
use managarm::svrctl::hardware_access_handle;
use std::collections::{BTreeMap, VecDeque};
use std::ffi::{CStr, c_void};
use std::sync::atomic::Ordering;
use std::time::{Duration, Instant};

use lock_api::{RawMutex as _, RawMutexTimed as _};
use parking_lot::{Condvar, Mutex, RawMutex};

use uacpi_sys::{
    uacpi_bool, uacpi_char, uacpi_cpu_flags, uacpi_firmware_request, uacpi_handle,
    uacpi_interrupt_handler, uacpi_interrupt_state, uacpi_io_addr, uacpi_log_level,
    uacpi_pci_address, uacpi_phys_addr, uacpi_size, uacpi_status, uacpi_thread_id, uacpi_u8,
    uacpi_u16, uacpi_u32, uacpi_u64, uacpi_work_handler, uacpi_work_type,
};

use super::{PAGE_MASK, RSDP};
use crate::pci::config;

const UACPI_MAP_FAILED: *mut c_void = (-1isize) as *mut c_void;

unsafe extern "C" {
    unsafe fn malloc(size: usize) -> *mut c_void;
    unsafe fn free(ptr: *mut c_void);
}

struct Mappings {
    map: BTreeMap<usize, hel::Mapping<u8>>,
}

static MAPPINGS: Mutex<Mappings> = Mutex::new(Mappings {
    map: BTreeMap::new(),
});

struct WorkItem {
    handler: unsafe extern "C" fn(uacpi_handle),
    ctx: usize,
}

static WORK_QUEUE: Mutex<VecDeque<WorkItem>> = Mutex::new(VecDeque::new());

fn drain_work() {
    loop {
        let item = WORK_QUEUE.lock().pop_front();
        let Some(item) = item else {
            break;
        };
        unsafe { (item.handler)(item.ctx as uacpi_handle) };
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_get_rsdp(out: *mut uacpi_phys_addr) -> uacpi_status {
    let rsdp = RSDP.load(Ordering::Relaxed);
    if rsdp == 0 {
        return uacpi_sys::UACPI_STATUS_NOT_FOUND;
    }
    unsafe { *out = rsdp as uacpi_phys_addr };
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_map(addr: uacpi_phys_addr, len: uacpi_size) -> *mut c_void {
    let page_off = (addr as usize) & PAGE_MASK;
    let aligned = (addr as usize) & !PAGE_MASK;
    let span = (len + page_off + PAGE_MASK) & !PAGE_MASK;

    let handle = match hel::access_physical(
        hardware_access_handle(),
        aligned,
        span,
        hel::CachingMode::Default,
    ) {
        Ok(handle) => handle,
        Err(_) => return UACPI_MAP_FAILED,
    };
    let mapping = match unsafe {
        hel::Mapping::<u8>::new(
            &handle,
            None,
            0,
            span,
            hel::MappingFlags::READ | hel::MappingFlags::WRITE,
        )
    } {
        Ok(mapping) => mapping,
        Err(_) => return UACPI_MAP_FAILED,
    };

    let base = match unsafe { mapping.as_ptr() } {
        Some(base) => base.as_ptr(),
        None => return UACPI_MAP_FAILED,
    };

    MAPPINGS.lock().map.insert(base as usize, mapping);
    unsafe { base.byte_add(page_off) as *mut c_void }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_unmap(addr: *mut c_void, _len: uacpi_size) {
    let base = (addr as usize) & !PAGE_MASK;
    MAPPINGS.lock().map.remove(&base);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_log(level: uacpi_log_level, msg: *const uacpi_char) {
    let text = unsafe { CStr::from_ptr(msg) };
    let tag = match level {
        uacpi_sys::UACPI_LOG_TRACE => "trace",
        uacpi_sys::UACPI_LOG_INFO => "info",
        uacpi_sys::UACPI_LOG_WARN => "warn",
        uacpi_sys::UACPI_LOG_ERROR => "error",
        _ => "debug",
    };
    eprint!("sif: uacpi-{tag}: {}", text.to_string_lossy());
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_alloc(size: uacpi_size) -> *mut c_void {
    unsafe { malloc(size) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_free(mem: *mut c_void) {
    unsafe { free(mem) }
}

// Ensure that we can store a PCI address inside the handle instead of allocating memory.
const _: () = {
    assert!(std::mem::size_of::<uacpi_pci_address>() <= std::mem::size_of::<uacpi_handle>());
};

unsafe fn pack_address(address: uacpi_pci_address) -> uacpi_handle {
    let mut value: usize = 0;
    unsafe {
        std::ptr::copy_nonoverlapping(
            &address as *const _ as *const u8,
            &mut value as *mut _ as *mut u8,
            std::mem::size_of::<uacpi_pci_address>(),
        );
    }
    value as uacpi_handle
}

unsafe fn unpack_address(handle: uacpi_handle) -> uacpi_pci_address {
    let value = handle as usize;
    let mut address = uacpi_pci_address::default();
    unsafe {
        std::ptr::copy_nonoverlapping(
            &value as *const _ as *const u8,
            &mut address as *mut _ as *mut u8,
            std::mem::size_of::<uacpi_pci_address>(),
        );
    }
    address
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_pci_device_open(
    address: uacpi_pci_address,
    out: *mut uacpi_handle,
) -> uacpi_status {
    unsafe { *out = pack_address(address) };
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_pci_device_close(_handle: uacpi_handle) {}

/// AML addresses devices and registers that need not be there at all.
fn is_absent(error: &config::ConfigIoError) -> bool {
    matches!(
        error,
        config::ConfigIoError::NoConfigSpace { .. } | config::ConfigIoError::OutOfRange { .. }
    )
}

/// Completes a configuration space read, substituting all-ones for absent registers.
///
/// # Safety
///
/// `value` has to be valid for writes.
unsafe fn complete_read<T>(result: config::Result<T>, value: *mut T, absent: T) -> uacpi_status {
    match result {
        Ok(read) => {
            unsafe { *value = read };
            uacpi_sys::UACPI_STATUS_OK
        }
        Err(error) if is_absent(&error) => {
            unsafe { *value = absent };
            uacpi_sys::UACPI_STATUS_OK
        }
        Err(error) => {
            println!("sif: uacpi: Configuration space read failed: {error}");
            uacpi_sys::UACPI_STATUS_INTERNAL_ERROR
        }
    }
}

/// Completes a configuration space write, discarding writes to absent registers.
fn complete_write(result: config::Result<()>) -> uacpi_status {
    match result {
        Ok(()) => uacpi_sys::UACPI_STATUS_OK,
        Err(error) if is_absent(&error) => uacpi_sys::UACPI_STATUS_OK,
        Err(error) => {
            println!("sif: uacpi: Configuration space write failed: {error}");
            uacpi_sys::UACPI_STATUS_INTERNAL_ERROR
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_pci_read8(
    device: uacpi_handle,
    offset: uacpi_size,
    value: *mut uacpi_u8,
) -> uacpi_status {
    let a = unsafe { unpack_address(device) };
    let read =
        unsafe { config::read_config_byte(a.segment, a.bus, a.device, a.function, offset as u16) };
    unsafe { complete_read(read, value, 0xFF) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_pci_read16(
    device: uacpi_handle,
    offset: uacpi_size,
    value: *mut uacpi_u16,
) -> uacpi_status {
    let a = unsafe { unpack_address(device) };
    let read =
        unsafe { config::read_config_half(a.segment, a.bus, a.device, a.function, offset as u16) };
    unsafe { complete_read(read, value, 0xFFFF) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_pci_read32(
    device: uacpi_handle,
    offset: uacpi_size,
    value: *mut uacpi_u32,
) -> uacpi_status {
    let a = unsafe { unpack_address(device) };
    let read =
        unsafe { config::read_config_word(a.segment, a.bus, a.device, a.function, offset as u16) };
    unsafe { complete_read(read, value, 0xFFFFFFFF) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_pci_write8(
    device: uacpi_handle,
    offset: uacpi_size,
    value: uacpi_u8,
) -> uacpi_status {
    let a = unsafe { unpack_address(device) };
    complete_write(unsafe {
        config::write_config_byte(a.segment, a.bus, a.device, a.function, offset as u16, value)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_pci_write16(
    device: uacpi_handle,
    offset: uacpi_size,
    value: uacpi_u16,
) -> uacpi_status {
    let a = unsafe { unpack_address(device) };
    complete_write(unsafe {
        config::write_config_half(a.segment, a.bus, a.device, a.function, offset as u16, value)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_pci_write32(
    device: uacpi_handle,
    offset: uacpi_size,
    value: uacpi_u32,
) -> uacpi_status {
    let a = unsafe { unpack_address(device) };
    complete_write(unsafe {
        config::write_config_word(a.segment, a.bus, a.device, a.function, offset as u16, value)
    })
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_io_map(
    base: uacpi_io_addr,
    len: uacpi_size,
    out: *mut uacpi_handle,
) -> uacpi_status {
    // The window is named by AML, hence it can lie outside of the 16-bit port space entirely.
    let fits = usize::try_from(base)
        .ok()
        .and_then(|base| base.checked_add(len))
        .is_some_and(|end| end <= 1 << 16);
    if !fits {
        return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT;
    }

    #[cfg(target_arch = "x86_64")]
    {
        let mut ports = Vec::with_capacity(len);
        for i in 0..len {
            ports.push(base as usize + i);
        }

        let Ok(handle) = hel::access_io(hardware_access_handle(), &ports) else {
            return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT;
        };
        let Ok(()) = hel::enable_io(handle) else {
            return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT;
        };
    }
    unsafe { *out = base as uacpi_handle };
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_io_unmap(_handle: uacpi_handle) {}

/// The handle that `uacpi_kernel_io_map` hands out is the base of the mapped window.
///
/// The offset is named by AML, so it is checked against the port space rather than trusted.
fn pio_space<T: PioAccess>(handle: uacpi_handle, offset: uacpi_size) -> Option<PioSpace> {
    // uacpi_kernel_io_map rejects a base that does not fit, and enables the ports of the window.
    let space = unsafe { PioSpace::new(handle as usize as u16) };

    space.access_ok::<T>(offset).then_some(space)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_io_read8(
    handle: uacpi_handle,
    offset: uacpi_size,
    out: *mut uacpi_u8,
) -> uacpi_status {
    let Some(space) = pio_space::<u8>(handle, offset) else {
        return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT;
    };

    unsafe { *out = space.scalar_load::<u8>(offset) };
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_io_read16(
    handle: uacpi_handle,
    offset: uacpi_size,
    out: *mut uacpi_u16,
) -> uacpi_status {
    let Some(space) = pio_space::<u16>(handle, offset) else {
        return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT;
    };

    unsafe { *out = space.scalar_load::<u16>(offset) };
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_io_read32(
    handle: uacpi_handle,
    offset: uacpi_size,
    out: *mut uacpi_u32,
) -> uacpi_status {
    let Some(space) = pio_space::<u32>(handle, offset) else {
        return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT;
    };

    unsafe { *out = space.scalar_load::<u32>(offset) };
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_io_write8(
    handle: uacpi_handle,
    offset: uacpi_size,
    value: uacpi_u8,
) -> uacpi_status {
    let Some(space) = pio_space::<u8>(handle, offset) else {
        return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT;
    };

    unsafe { space.scalar_store::<u8>(offset, value) };
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_io_write16(
    handle: uacpi_handle,
    offset: uacpi_size,
    value: uacpi_u16,
) -> uacpi_status {
    let Some(space) = pio_space::<u16>(handle, offset) else {
        return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT;
    };

    unsafe { space.scalar_store::<u16>(offset, value) };
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_io_write32(
    handle: uacpi_handle,
    offset: uacpi_size,
    value: uacpi_u32,
) -> uacpi_status {
    let Some(space) = pio_space::<u32>(handle, offset) else {
        return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT;
    };

    unsafe { space.scalar_store::<u32>(offset, value) };
    uacpi_sys::UACPI_STATUS_OK
}

fn clock_now() -> u64 {
    hel::Time::new_since_boot()
        .map(|time| time.nanos())
        .unwrap()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_get_nanoseconds_since_boot() -> uacpi_u64 {
    clock_now()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_stall(usec: uacpi_u8) {
    let deadline = clock_now() + (usec as u64) * 1000;
    while clock_now() < deadline {}
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_sleep(msec: uacpi_u64) {
    hel::executor::block_on(hel::sleep_for(Duration::from_millis(msec)))
        .flatten()
        .unwrap();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_create_mutex() -> uacpi_handle {
    Box::into_raw(Box::new(RawMutex::INIT)) as uacpi_handle
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_free_mutex(handle: uacpi_handle) {
    unsafe { drop(Box::from_raw(handle as *mut RawMutex)) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_acquire_mutex(
    handle: uacpi_handle,
    timeout: uacpi_u16,
) -> uacpi_status {
    let mutex = unsafe { &*(handle as *const RawMutex) };
    let acquired = match timeout {
        0xFFFF => {
            mutex.lock();
            true
        }
        0 => mutex.try_lock(),
        ms => mutex.try_lock_for(Duration::from_millis(ms as u64)),
    };
    if acquired {
        uacpi_sys::UACPI_STATUS_OK
    } else {
        uacpi_sys::UACPI_STATUS_TIMEOUT
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_release_mutex(handle: uacpi_handle) {
    let mutex = unsafe { &*(handle as *const RawMutex) };
    unsafe { mutex.unlock() };
}

struct Event {
    count: Mutex<u64>,
    cond: Condvar,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_create_event() -> uacpi_handle {
    Box::into_raw(Box::new(Event {
        count: Mutex::new(0),
        cond: Condvar::new(),
    })) as uacpi_handle
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_free_event(handle: uacpi_handle) {
    unsafe { drop(Box::from_raw(handle as *mut Event)) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_wait_for_event(
    handle: uacpi_handle,
    timeout: uacpi_u16,
) -> uacpi_bool {
    let event = unsafe { &*(handle as *const Event) };
    let mut count = event.count.lock();

    if *count == 0 {
        match timeout {
            0 => return false,
            0xFFFF => {
                while *count == 0 {
                    event.cond.wait(&mut count);
                }
            }
            ms => {
                let deadline = Instant::now() + Duration::from_millis(ms as u64);
                while *count == 0 {
                    if event.cond.wait_until(&mut count, deadline).timed_out() {
                        if *count == 0 {
                            return false;
                        }
                        break;
                    }
                }
            }
        }
    }

    *count -= 1;
    true
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_signal_event(handle: uacpi_handle) {
    let event = unsafe { &*(handle as *const Event) };
    *event.count.lock() += 1;
    event.cond.notify_one();
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_reset_event(handle: uacpi_handle) {
    let event = unsafe { &*(handle as *const Event) };
    *event.count.lock() = 0;
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_get_thread_id() -> uacpi_thread_id {
    1usize as uacpi_thread_id
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_disable_interrupts() -> uacpi_interrupt_state {
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_restore_interrupts(_state: uacpi_interrupt_state) {}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_create_spinlock() -> uacpi_handle {
    Box::into_raw(Box::new(RawMutex::INIT)) as uacpi_handle
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_free_spinlock(handle: uacpi_handle) {
    unsafe { drop(Box::from_raw(handle as *mut RawMutex)) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_lock_spinlock(handle: uacpi_handle) -> uacpi_cpu_flags {
    let lock = unsafe { &*(handle as *const RawMutex) };
    lock.lock();
    0
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_unlock_spinlock(
    handle: uacpi_handle,
    _flags: uacpi_cpu_flags,
) {
    let lock = unsafe { &*(handle as *const RawMutex) };
    unsafe { lock.unlock() };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_schedule_work(
    _type: uacpi_work_type,
    handler: uacpi_work_handler,
    ctx: uacpi_handle,
) -> uacpi_status {
    if let Some(handler) = handler {
        WORK_QUEUE.lock().push_back(WorkItem {
            handler,
            ctx: ctx as usize,
        });
    }
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_wait_for_work_completion() -> uacpi_status {
    drain_work();
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_install_interrupt_handler(
    _irq: uacpi_u32,
    _handler: uacpi_interrupt_handler,
    _ctx: uacpi_handle,
    out: *mut uacpi_handle,
) -> uacpi_status {
    unsafe { *out = std::ptr::null_mut() };
    // TODO
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_uninstall_interrupt_handler(
    _handler: uacpi_interrupt_handler,
    _irq_handle: uacpi_handle,
) -> uacpi_status {
    // TODO
    uacpi_sys::UACPI_STATUS_OK
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn uacpi_kernel_handle_firmware_request(
    _request: *mut uacpi_firmware_request,
) -> uacpi_status {
    println!("sif: ignoring ACPI firmware request");
    // TODO
    uacpi_sys::UACPI_STATUS_OK
}
