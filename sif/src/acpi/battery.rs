use std::{
    cell::RefCell,
    collections::HashMap,
    ffi::CStr,
    future::Future,
    pin::Pin,
    sync::{Arc, Mutex},
    task::{Context, Poll, Waker},
};

use anyhow::Result;
use managarm::hw::server::{Battery, BatteryState as HwBatteryState, serve_battery};
use managarm::mbus::create_entity;
use uacpi_sys::{uacpi_handle, uacpi_u64};

use crate::acpi::object;
use crate::entity::{decimal, serve_entity_lanes, string};
use crate::leak;
use crate::uacpi::handlers;
use crate::uacpi::namespace::{self, IterationDecision, NamespaceNode};

const HID_BATTERY: &CStr = c"PNP0C0A";
const EXPECT_LOCK: &str = "sif: battery state mutex was poisoned";
const UNKNOWN: u64 = 0xFFFFFFFF;

mod bif {
    pub mod power_unit {
        pub const MILLIWATT: u64 = 0;
        pub const MILLIAMPERE: u64 = 1;
    }
}

mod bst {
    pub mod state {
        pub const DISCHARGING: u64 = 1 << 0;
        pub const CHARGING: u64 = 1 << 1;
        pub const CRITICAL_ENERGY_STATE: u64 = 1 << 2;
        pub const CHARGE_LIMITING: u64 = 1 << 3;
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PowerUnit {
    Milliwatt,
    Milliampere,
}

impl PowerUnit {
    fn from_acpi(units: u64) -> Option<PowerUnit> {
        match units {
            bif::power_unit::MILLIWATT => Some(PowerUnit::Milliwatt),
            bif::power_unit::MILLIAMPERE => Some(PowerUnit::Milliampere),
            _ => None,
        }
    }
}

fn milliwatthours(units: PowerUnit, capacity: u32, voltage: Option<u32>) -> Option<u32> {
    match units {
        PowerUnit::Milliwatt => Some(capacity),
        PowerUnit::Milliampere => voltage.map(|voltage| capacity * voltage),
    }
}

#[derive(Default)]
struct BatteryState {
    units: Option<PowerUnit>,

    charging: bool,
    rate_milliwatt: Option<u32>,
    rate_milliampere: Option<u32>,
    voltage: Option<u32>,
    design_voltage: Option<u32>,

    remaining_capacity_milliwatthours: Option<u32>,
    design_capacity_milliwatthours: Option<u32>,
    last_full_charge_capacity_milliwatthours: Option<u32>,
}

impl BatteryState {
    fn update_bif(&mut self, node: NamespaceNode) -> Result<()> {
        let bif = match node.eval_package(c"_BIF") {
            Ok(bif) => bif,
            Err(err) => {
                println!("sif: acpi: battery _BIF error: {err}");
                return Ok(());
            }
        };
        let bif = bif.package()?;

        self.units = bif.integer(0).and_then(PowerUnit::from_acpi);

        self.design_voltage = bif
            .integer(4)
            .filter(|&voltage| voltage != UNKNOWN)
            .map(|voltage| voltage as u32);

        let design_capacity = bif.integer(1).filter(|&capacity| capacity != UNKNOWN);
        self.design_capacity_milliwatthours = match (self.units, design_capacity) {
            (Some(units), Some(capacity)) => milliwatthours(units, capacity as u32, self.voltage),
            _ => None,
        };

        let last_full_charge_capacity = bif.integer(2).filter(|&capacity| capacity != UNKNOWN);
        self.last_full_charge_capacity_milliwatthours =
            match (self.units, last_full_charge_capacity) {
                (Some(units), Some(capacity)) => {
                    milliwatthours(units, capacity as u32, self.voltage)
                }
                _ => None,
            };

        Ok(())
    }

    fn update_bst(&mut self, node: NamespaceNode) -> Result<()> {
        let bst = match node.eval_package(c"_BST") {
            Ok(bst) => bst,
            Err(err) => {
                println!("sif: acpi: battery _BST error: {err}");
                return Ok(());
            }
        };
        let bst = bst.package()?;

        if let Some(state) = bst.integer(0) {
            if state & bst::state::DISCHARGING != 0 {
                self.charging = false;
            }
            if state & bst::state::CHARGING != 0 {
                self.charging = true;
            }
            if state & bst::state::CRITICAL_ENERGY_STATE != 0 {
                println!("sif: acpi: battery state: critical energy");
            }
            if state & bst::state::CHARGE_LIMITING != 0 {
                println!("sif: acpi: battery state: charge limiting");
            }
        }

        self.voltage = bst
            .integer(3)
            .filter(|&voltage| voltage != UNKNOWN)
            .map(|voltage| voltage as u32);

        let rate = bst.integer(1).filter(|&rate| rate != UNKNOWN);
        match (self.units, rate) {
            (Some(PowerUnit::Milliampere), Some(rate)) => {
                let rate = (rate as i32).unsigned_abs();
                self.rate_milliampere = Some(rate);
                self.rate_milliwatt = self.voltage.map(|voltage| rate * voltage);
            }
            (Some(PowerUnit::Milliwatt), Some(rate)) => {
                let rate = rate as u32;
                self.rate_milliwatt = Some(rate);
                self.rate_milliampere = self.voltage.map(|voltage| rate / voltage);
            }
            _ => {
                self.rate_milliwatt = None;
                self.rate_milliampere = None;
            }
        }

        let remaining_capacity = bst.integer(2).filter(|&capacity| capacity != UNKNOWN);
        self.remaining_capacity_milliwatthours = match (self.units, remaining_capacity) {
            (Some(units), Some(capacity)) => milliwatthours(units, capacity as u32, self.voltage),
            _ => None,
        };

        Ok(())
    }

    fn update(&mut self, node: NamespaceNode) -> Result<()> {
        self.update_bif(node)?;
        self.update_bst(node)
    }

    fn to_protocol(&self) -> HwBatteryState {
        let scale = |value: Option<u32>| value.map(|value| u64::from(value) * 1000);

        HwBatteryState {
            charging: self.charging,
            current_now: scale(self.rate_milliampere),
            power_now: scale(self.rate_milliwatt),
            energy_now: scale(self.remaining_capacity_milliwatthours),
            energy_full: scale(self.last_full_charge_capacity_milliwatthours),
            energy_full_design: scale(self.design_capacity_milliwatthours),
            voltage_now: scale(self.voltage),
            voltage_min_design: scale(self.design_voltage),
        }
    }
}

struct Event {
    wakers: RefCell<Vec<Waker>>,
}

impl Event {
    fn new() -> Event {
        Event {
            wakers: RefCell::new(Vec::new()),
        }
    }

    fn raise(&self) {
        for waker in self.wakers.borrow_mut().drain(..) {
            waker.wake();
        }
    }

    fn wait(&self) -> Wait<'_> {
        Wait {
            event: self,
            registered: false,
        }
    }
}

struct Wait<'a> {
    event: &'a Event,
    registered: bool,
}

impl Future for Wait<'_> {
    type Output = ();

    fn poll(mut self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<()> {
        if self.registered {
            return Poll::Ready(());
        }

        self.event.wakers.borrow_mut().push(cx.waker().clone());
        self.registered = true;
        Poll::Pending
    }
}

struct BatteryObject {
    id: usize,
    node: NamespaceNode,
    state: Mutex<BatteryState>,
    event: Event,
}

impl BatteryObject {
    fn new(node: NamespaceNode, id: usize) -> BatteryObject {
        BatteryObject {
            id,
            node,
            state: Mutex::new(BatteryState::default()),
            event: Event::new(),
        }
    }

    fn update(&self) {
        let mut state = self.state.lock().expect(EXPECT_LOCK);
        if let Err(err) = state.update(self.node) {
            println!("sif: acpi: battery {} update failed: {err}", self.id);
            return;
        }

        println!(
            "sif: acpi: battery {}: {}, {:?} of {:?} mWh at {:?} mV, {:?} mW",
            self.id,
            if state.charging {
                "charging"
            } else {
                "discharging"
            },
            state.remaining_capacity_milliwatthours,
            state.last_full_charge_capacity_milliwatthours,
            state.voltage,
            state.rate_milliwatt,
        );
    }
}

impl Battery for BatteryObject {
    async fn state(&self, block_until_ready: bool) -> HwBatteryState {
        if block_until_ready {
            self.event.wait().await;
        }
        self.state.lock().expect(EXPECT_LOCK).to_protocol()
    }
}

unsafe extern "C" fn notification(
    context: uacpi_handle,
    _node: *mut uacpi_sys::uacpi_namespace_node,
    value: uacpi_u64,
) -> uacpi_sys::uacpi_status {
    let battery = unsafe { &*(context as *const BatteryObject) };
    println!(
        "sif: acpi: battery {} received AML Notify({value})",
        battery.id
    );

    battery.update();
    battery.event.raise();

    uacpi_sys::UACPI_STATUS_OK
}

async fn publish_battery(node: NamespaceNode, id: usize) -> Result<()> {
    // sif's tasks are not Send, but neither are the objects that they serve.
    #[allow(clippy::arc_with_non_send_sync)]
    let battery = Arc::new(BatteryObject::new(node, id));
    battery.update();

    let parent = object::publish(node, id).await?;

    let mut props = HashMap::new();
    props.insert("class".into(), string("power_supply"));
    props.insert("power_supply.type".into(), string("battery"));
    props.insert("power_supply.id".into(), string(&id.to_string()));
    props.insert("drvcore.mbus-parent".into(), decimal(parent.id()));

    // We need to leak because uACPI keeps the handler forever.
    let context: &'static Arc<BatteryObject> = leak(Arc::clone(&battery));
    handlers::install_notify_handler(
        node,
        Some(notification),
        Arc::as_ptr(context) as uacpi_handle,
    )?;

    println!("sif: acpi: publishing battery {id}");
    let manager = leak(create_entity("battery", &props).await?);
    hel::spawn(serve_entity_lanes(manager, move |lane| {
        hel::spawn(serve_battery(lane, Arc::clone(&battery)));
    }));

    Ok(())
}

pub async fn publish() -> Result<()> {
    let mut nodes = Vec::new();
    namespace::find_devices_at(NamespaceNode::root(), &[HID_BATTERY], |node| {
        if node.find(c"_BIF").ok().flatten().is_some()
            && node.find(c"_BST").ok().flatten().is_some()
        {
            nodes.push(node);
        }
        IterationDecision::Continue
    })?;

    let count = nodes.len();
    for (id, node) in nodes.into_iter().enumerate() {
        publish_battery(node, id).await?;
    }

    if count == 0 {
        println!("sif: acpi: no battery devices on the system");
    }

    Ok(())
}
