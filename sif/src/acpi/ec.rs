use std::ffi::{CStr, CString};
use std::sync::OnceLock;
use std::sync::atomic::{AtomicBool, Ordering};

use anyhow::{Result, bail};
use event_listener::Event;
use uacpi_sys::acpi_ecdt;

use crate::leak;
use crate::uacpi::handlers::{self, RegionError, RegionOp, RegionRw};
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

    fn transfer(&self, op: RegionOp, access: &mut RegionRw<'_>) -> Result<()> {
        let offset = access.offset() as u8;

        self.burst_enable()?;
        let result = match op {
            RegionOp::Read => self
                .read(offset)
                .map(|value| access.set_value(u64::from(value))),
            RegionOp::Write => self.write(offset, access.value() as u8),
        };
        self.burst_disable()?;

        result
    }
}

fn handle_region(
    device: &EcDevice,
    op: RegionOp,
    access: &mut RegionRw<'_>,
) -> std::result::Result<(), RegionError> {
    if access.byte_width() != 1 {
        println!("sif: acpi: invalid EC access width {}", access.byte_width());
        return Err(RegionError::InvalidArgument);
    }

    device.transfer(op, access).map_err(|err| {
        println!("sif: acpi: EC access failed: {err}");
        RegionError::HardwareTimeout
    })
}

fn handle_gpe() {
    // Running AML from the IRQ path is unsafe, hence defer the query to the worker.
    println!("sif: acpi: EC GPE fired");
    EVENT_QUEUED.store(true, Ordering::Release);
    EVENT.notify(1);
}

static EVENT_QUEUED: AtomicBool = AtomicBool::new(false);
static EVENT: Event = Event::new();
static EC_WORKER_SPAWNED: AtomicBool = AtomicBool::new(false);

/// Waits until the EC GPE handler queued an event.
async fn wait_for_event() {
    loop {
        // Register before checking the flag to avoid a lost wakeup.
        let listener = EVENT.listen();
        if EVENT_QUEUED.swap(false, Ordering::AcqRel) {
            return;
        }
        listener.await;
    }
}

/// Handles EC events in task context, i.e., runs the _Qxx query methods.
async fn run_ec_events(device: &'static EcDevice) {
    loop {
        wait_for_event().await;

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

static EC: OnceLock<&'static EcDevice> = OnceLock::new();
static HANDLERS_INSTALLED: AtomicBool = AtomicBool::new(false);

fn install_handlers(device: &'static EcDevice) -> Result<()> {
    handlers::install_address_space_handler(
        device.node,
        uacpi_sys::UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
        move |op, access| handle_region(device, op, access),
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
        handle_gpe,
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

    Ok(())
}
