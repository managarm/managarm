//! The ACPI Embedded Controller; port of thor's system/acpi/ec.cpp.

use std::cell::Cell;
use std::ffi::{CStr, CString};
use std::future::Future;
use std::pin::Pin;
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};
use std::task::{Context, Poll, Waker};
use std::time::Duration;

use anyhow::{Result, bail};
use uacpi_sys::{acpi_ecdt, uacpi_handle, uacpi_region_rw_data};

use crate::leak;
use crate::uacpi::handlers;
use crate::uacpi::io::Gas;
use crate::uacpi::namespace::{self, IterationDecision, NamespaceNode};
use crate::uacpi::resources::Resource;
use crate::uacpi::table::Table;

const HID_EC: &CStr = c"PNP0C09";

const EC_OBF: u8 = 1 << 0;
const EC_IBF: u8 = 1 << 1;
const EC_BURST: u8 = 1 << 4;
const EC_SCI_EVT: u8 = 1 << 5;

const RD_EC: u8 = 0x80;
const WR_EC: u8 = 0x81;
const BE_EC: u8 = 0x82;
const BD_EC: u8 = 0x83;
const QR_EC: u8 = 0x84;

const BURST_ACK: u8 = 0x90;

struct EcDevice {
    node: NamespaceNode,
    control: Gas,
    data: Gas,
    gpe_index: OnceLock<u16>,
}

impl EcDevice {
    fn new(node: NamespaceNode, control: Gas, data: Gas) -> EcDevice {
        EcDevice {
            node,
            control,
            data,
            gpe_index: OnceLock::new(),
        }
    }

    fn wait_for_bit(&self, register: &Gas, bit: u8, value: bool) -> Result<()> {
        while (register.read()? as u8 & bit != 0) != value {}
        Ok(())
    }

    fn write_one(&self, register: &Gas, value: u8) -> Result<()> {
        self.wait_for_bit(&self.control, EC_IBF, false)?;
        register.write(u64::from(value))?;
        Ok(())
    }

    fn read_one(&self, register: &Gas) -> Result<u8> {
        self.wait_for_bit(&self.control, EC_OBF, true)?;
        Ok(register.read()? as u8)
    }

    fn burst_enable(&self) -> Result<()> {
        self.write_one(&self.control, BE_EC)?;
        let acknowledge = self.read_one(&self.data)?;
        if acknowledge != BURST_ACK {
            bail!("sif: acpi: EC answered a burst enable with {acknowledge:#04x}");
        }
        Ok(())
    }

    fn burst_disable(&self) -> Result<()> {
        self.write_one(&self.control, BD_EC)?;
        self.wait_for_bit(&self.control, EC_BURST, false)
    }

    fn read(&self, offset: u8) -> Result<u8> {
        self.write_one(&self.control, RD_EC)?;
        self.write_one(&self.data, offset)?;
        self.read_one(&self.data)
    }

    fn write(&self, offset: u8, value: u8) -> Result<()> {
        self.write_one(&self.control, WR_EC)?;
        self.write_one(&self.data, offset)?;
        self.write_one(&self.data, value)
    }

    fn check_event(&self) -> Result<Option<u8>> {
        let status = self.control.read()? as u8;

        // We get an extra EC event when disabling burst, that's ok.
        if status & EC_SCI_EVT == 0 {
            return Ok(None);
        }

        self.burst_enable()?;
        self.write_one(&self.control, QR_EC)?;
        let index = self.read_one(&self.data)?;
        self.burst_disable()?;

        Ok(Some(index))
    }

    fn handle_event(&self) -> Result<()> {
        let Some(index) = self.check_event()? else {
            return Ok(());
        };
        if index == 0 {
            return Ok(());
        }

        let method = CString::new(format!("_Q{index:02X}")).expect("EC query is not a method name");
        println!("sif: acpi: running EC query {method:?}");
        self.node.execute(&method)?;
        Ok(())
    }

    fn transfer(&self, read: bool, data: &mut uacpi_region_rw_data) -> Result<()> {
        // SAFETY: uACPI passes the offset of the access in the union of the address.
        let offset = unsafe { data.__bindgen_anon_1.offset } as u8;

        self.burst_enable()?;
        let result = if read {
            self.read(offset).map(|value| data.value = u64::from(value))
        } else {
            self.write(offset, data.value as u8)
        };
        self.burst_disable()?;

        result
    }
}

unsafe extern "C" fn handle_region(
    op: uacpi_sys::uacpi_region_op,
    data: uacpi_handle,
) -> uacpi_sys::uacpi_status {
    match op {
        uacpi_sys::UACPI_REGION_OP_ATTACH | uacpi_sys::UACPI_REGION_OP_DETACH => {
            return uacpi_sys::UACPI_STATUS_OK;
        }
        uacpi_sys::UACPI_REGION_OP_READ | uacpi_sys::UACPI_REGION_OP_WRITE => (),
        _ => return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT,
    }

    // SAFETY: uACPI passes a uacpi_region_rw_data for reads and writes.
    let data = unsafe { &mut *(data as *mut uacpi_region_rw_data) };
    if data.byte_width != 1 {
        println!("sif: acpi: invalid EC access width {}", data.byte_width);
        return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT;
    }

    // SAFETY: uACPI passes back the context that install_handlers() handed to it.
    let device = unsafe { &*(data.handler_context as *const EcDevice) };
    match device.transfer(op == uacpi_sys::UACPI_REGION_OP_READ, data) {
        Ok(()) => uacpi_sys::UACPI_STATUS_OK,
        Err(err) => {
            println!("sif: acpi: EC access failed: {err}");
            uacpi_sys::UACPI_STATUS_HARDWARE_TIMEOUT
        }
    }
}

unsafe extern "C" fn handle_gpe(
    _context: uacpi_handle,
    _gpe_device: *mut uacpi_sys::uacpi_namespace_node,
    _index: uacpi_sys::uacpi_u16,
) -> uacpi_sys::uacpi_interrupt_ret {
    // Running AML from the IRQ path is unsafe, hence defer the query to the worker.
    println!("sif: acpi: EC GPE fired");
    EVENT_QUEUED.store(true, Ordering::Release);
    EVENT_WAKER.with(|waker| {
        if let Some(waker) = waker.take() {
            waker.wake();
        }
    });

    uacpi_sys::UACPI_INTERRUPT_HANDLED
}

static EVENT_QUEUED: AtomicBool = AtomicBool::new(false);
static EC_WORKER_SPAWNED: AtomicBool = AtomicBool::new(false);
static EC_POLLER_SPAWNED: AtomicBool = AtomicBool::new(false);

thread_local! {
    static EVENT_WAKER: Cell<Option<Waker>> = const { Cell::new(None) };
}

/// Handles EC events in task context, i.e., runs the _Qxx query methods.
async fn run_ec_events(device: &'static EcDevice) {
    loop {
        EcWait.await;

        if let Err(err) = device.handle_event() {
            println!("sif: acpi: failed to handle an EC event: {err}");
        }
        if let Some(index) = device.gpe_index.get().copied()
            && let Err(err) = handlers::finish_handling_gpe(None, index)
        {
            println!("sif: acpi: failed to finish handling EC GPE {index}: {err}");
        }
    }
}

/// Polls the EC for events; this is a fallback for systems where the EC's SCI
/// does not reach us. The EC's SCI_EVT bit is the authoritative event source.
async fn poll_ec_events(device: &'static EcDevice) {
    loop {
        if let Err(err) = hel::sleep_for(Duration::from_millis(500)).await {
            println!("sif: acpi: failed to sleep while polling the EC: {err}");
            return;
        }

        if let Err(err) = device.handle_event() {
            println!("sif: acpi: failed to poll an EC event: {err}");
        }
    }
}

/// Waits until the EC GPE handler queued an event.
struct EcWait;

impl Future for EcWait {
    type Output = ();

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if EVENT_QUEUED.swap(false, Ordering::AcqRel) {
            return Poll::Ready(());
        }

        EVENT_WAKER.with(|waker| waker.set(Some(cx.waker().clone())));

        // Re-check after registering the waker to avoid a lost wakeup.
        if EVENT_QUEUED.swap(false, Ordering::AcqRel) {
            EVENT_WAKER.with(|waker| {
                waker.take();
            });
            return Poll::Ready(());
        }

        Poll::Pending
    }
}

static EC: OnceLock<&'static EcDevice> = OnceLock::new();
static HANDLERS_INSTALLED: AtomicBool = AtomicBool::new(false);

fn install_handlers(device: &'static EcDevice) -> Result<()> {
    handlers::install_address_space_handler(
        device.node,
        uacpi_sys::UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
        Some(handle_region),
        device as *const EcDevice as uacpi_handle,
    )?;

    if device.node.eval_simple_integer(c"_GLK")?.unwrap_or(0) != 0 {
        println!("sif: acpi: EC requires locking (this is a TODO)");
    }

    let Some(index) = device.node.eval_simple_integer(c"_GPE")? else {
        println!("sif: acpi: EC has no associated _GPE");
        return Ok(());
    };
    let index = u16::try_from(index)?;
    device
        .gpe_index
        .set(index)
        .expect("sif: acpi: EC GPE installed twice");

    handlers::install_gpe_handler(
        None,
        index,
        uacpi_sys::UACPI_GPE_TRIGGERING_EDGE,
        Some(handle_gpe),
        device as *const EcDevice as uacpi_handle,
    )?;

    if !EC_WORKER_SPAWNED.swap(true, Ordering::AcqRel) {
        hel::spawn(run_ec_events(device));
    }

    HANDLERS_INSTALLED.store(true, Ordering::Relaxed);
    Ok(())
}

fn init_from_ecdt() -> Result<Option<EcDevice>> {
    let Some(table) = Table::find_by_signature(c"ECDT")? else {
        println!("sif: acpi: no ECDT detected");
        return Ok(None);
    };

    // The ECDT is packed, hence we can read it out of the bytes of the table.
    let Some(header) = table.bytes().get(..size_of::<acpi_ecdt>()) else {
        println!("sif: acpi: ignoring a truncated ECDT");
        return Ok(None);
    };
    // SAFETY: every byte pattern of the size of a packed structure is a valid value.
    let ecdt: acpi_ecdt = unsafe { std::ptr::read_unaligned(header.as_ptr().cast()) };

    // The path of the EC follows the fixed-size part of the table.
    let Ok(path) =
        CStr::from_bytes_until_nul(table.bytes().get(size_of::<acpi_ecdt>()..).unwrap_or(&[]))
    else {
        println!("sif: acpi: ECDT names an EC without a path");
        return Ok(None);
    };
    println!("sif: acpi: found ECDT, EC@{}", path.to_string_lossy());

    let Some(node) = NamespaceNode::root().find(path)? else {
        println!("sif: acpi: invalid EC path {}", path.to_string_lossy());
        return Ok(None);
    };

    Ok(Some(EcDevice::new(
        node,
        Gas::from_raw(ecdt.ec_control),
        Gas::from_raw(ecdt.ec_data),
    )))
}

fn init_from_namespace() -> Result<Option<EcDevice>> {
    let mut found = None;

    namespace::find_devices_at(NamespaceNode::root(), &[HID_EC], |node| {
        let Ok(resources) = node.current_resources() else {
            return IterationDecision::Continue;
        };

        // The EC names its data port first and its control port second.
        let mut registers = Vec::new();
        for resource in resources.iter() {
            let (address, length) = match resource {
                Resource::Io(io) => (u64::from(io.minimum()), u64::from(io.length())),
                Resource::FixedIo(io) => (u64::from(io.address()), u64::from(io.length())),
                _ => continue,
            };
            registers.push(Gas::new(
                uacpi_sys::UACPI_ADDRESS_SPACE_SYSTEM_IO,
                address,
                (length * 8) as u8,
            ));
            if registers.len() == 2 {
                break;
            }
        }

        if registers.len() != 2 {
            println!("sif: acpi: didn't find all needed resources for EC");
            return IterationDecision::Continue;
        }

        println!("sif: acpi: found an EC@{}", node.absolute_path());
        found = Some(EcDevice::new(node, registers[1], registers[0]));
        IterationDecision::Break
    })?;

    Ok(found)
}

pub fn init() -> Result<()> {
    let mut early_reg = true;
    let device = match init_from_ecdt()? {
        Some(device) => device,
        None => {
            early_reg = false;
            let Some(device) = init_from_namespace()? else {
                println!("sif: acpi: no EC devices on the system");
                return Ok(());
            };
            device
        }
    };

    let device: &'static EcDevice = leak(device);
    assert!(EC.set(device).is_ok(), "sif: acpi: EC initialized twice");

    if early_reg {
        install_handlers(device)?;
    }

    Ok(())
}

pub fn init_events() -> Result<()> {
    if let Err(err) = handlers::finalize_gpe_initialization() {
        println!("sif: acpi: failed to finalize the GPEs: {err}");
    }

    let Some(device) = EC.get().copied() else {
        return Ok(());
    };
    if !HANDLERS_INSTALLED.load(Ordering::Relaxed) {
        install_handlers(device)?;
    }

    if let Some(index) = device.gpe_index.get().copied() {
        println!("sif: acpi: enabling EC GPE {index}");
        if let Err(err) = handlers::enable_gpe(None, index) {
            println!("sif: acpi: failed to enable EC GPE {index}: {err}");
        }
    }

    if !EC_POLLER_SPAWNED.swap(true, Ordering::AcqRel) {
        hel::spawn(poll_ec_events(device));
    }

    Ok(())
}
