//! The power_supply class subsystem: batteries.
//!
//! The battery driver answers a blocking state request whenever the state
//! changes; every change is reflected in the attributes and a change uevent.

use std::sync::{Arc, Mutex};

use anyhow::Result;
use async_trait::async_trait;
use managarm::fs;
use managarm::hw::{self, BatteryState};
use managarm::mbus;

use crate::EXPECT_LOCK;
use crate::device::{DeviceKey, DeviceSpec, Membership, Model, WeakAnnouncedDevice};
use crate::subsystem::{Installed, observe, parent_ref, remote_lane, required_prop, string_prop};
use crate::sysfs::{Attribute, StaticAttribute};

/// An attribute that formats one field of the shared battery state.
struct StateAttribute {
    state: Arc<Mutex<BatteryState>>,
    format: fn(&BatteryState) -> String,
}

impl StateAttribute {
    fn new(state: &Arc<Mutex<BatteryState>>, format: fn(&BatteryState) -> String) -> Arc<Self> {
        Arc::new(Self {
            state: state.clone(),
            format,
        })
    }
}

#[async_trait(?Send)]
impl Attribute for StateAttribute {
    async fn show(&self) -> Result<Vec<u8>, fs::server::Error> {
        Ok((self.format)(&self.state.lock().expect(EXPECT_LOCK)).into_bytes())
    }
}

fn status(state: &BatteryState) -> &'static str {
    if state.charging {
        "Charging"
    } else {
        "Discharging"
    }
}

fn uevent_extras(name: &str, state: &BatteryState) -> Vec<(String, String)> {
    let mut extras = vec![
        ("POWER_SUPPLY_NAME".to_string(), name.to_string()),
        ("POWER_SUPPLY_TYPE".to_string(), "Battery".to_string()),
        ("POWER_SUPPLY_STATUS".to_string(), status(state).to_string()),
    ];
    // sysfs reports the driver's units; the uevent uses micro-units like Linux.
    let scaled = [
        ("POWER_SUPPLY_VOLTAGE_MIN_DESIGN", state.voltage_min_design),
        ("POWER_SUPPLY_VOLTAGE_NOW", state.voltage_now),
        ("POWER_SUPPLY_CURRENT_NOW", state.current_now),
        ("POWER_SUPPLY_ENERGY_NOW", state.energy_now),
        ("POWER_SUPPLY_ENERGY_FULL", state.energy_full),
        ("POWER_SUPPLY_ENERGY_FULL_DESIGN", state.energy_full_design),
    ];
    for (key, value) in scaled {
        if let Some(value) = value {
            extras.push((key.to_string(), (value * 1000).to_string()));
        }
    }
    extras
}

/// Reflects every state change of the battery until the device is gone.
async fn monitor(
    hw: hw::Device,
    state: Arc<Mutex<BatteryState>>,
    device: WeakAnnouncedDevice,
) -> Result<()> {
    loop {
        let update = hw.get_battery_state(true).await?;
        *state.lock().expect(EXPECT_LOCK) = update;
        if !device.change(&[]).await? {
            return Ok(());
        }
    }
}

async fn install_entity(
    model: Arc<Model>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let id = required_prop(properties, "power_supply.id")?;
    let name = format!("BAT{id}");

    let hw = hw::Device::new(remote_lane(&event).await?);
    let state = Arc::new(Mutex::new(hw.get_battery_state(false).await?));

    let uevent_state = state.clone();
    let uevent_name = name.clone();
    let mut spec = DeviceSpec::new(
        name,
        parent_ref(properties),
        Membership::class("power_supply"),
    )
    .key(DeviceKey::primary(event.entity_id()))
    .uevent_composer(move |env| {
        env.extend(uevent_extras(
            &uevent_name,
            &uevent_state.lock().expect(EXPECT_LOCK),
        ))
    })
    .attr("type", StaticAttribute::new("Battery\n"))
    .attr(
        "status",
        StateAttribute::new(&state, |s| format!("{}\n", status(s))),
    );
    // Only the fields that the battery reports get attributes.
    let optional: [(&str, fn(&BatteryState) -> Option<u64>); 7] = [
        ("current_now", |s| s.current_now),
        ("power_now", |s| s.power_now),
        ("energy_now", |s| s.energy_now),
        ("energy_full", |s| s.energy_full),
        ("energy_full_design", |s| s.energy_full_design),
        ("voltage_now", |s| s.voltage_now),
        ("voltage_min_design", |s| s.voltage_min_design),
    ];
    for (attr_name, field) in optional {
        if field(&state.lock().expect(EXPECT_LOCK)).is_none() {
            continue;
        }
        let format: fn(&BatteryState) -> String = match attr_name {
            "current_now" => |s: &BatteryState| format!("{}\n", s.current_now.unwrap_or(0)),
            "power_now" => |s: &BatteryState| format!("{}\n", s.power_now.unwrap_or(0)),
            "energy_now" => |s: &BatteryState| format!("{}\n", s.energy_now.unwrap_or(0)),
            "energy_full" => |s: &BatteryState| format!("{}\n", s.energy_full.unwrap_or(0)),
            "energy_full_design" => {
                |s: &BatteryState| format!("{}\n", s.energy_full_design.unwrap_or(0))
            }
            "voltage_now" => |s: &BatteryState| format!("{}\n", s.voltage_now.unwrap_or(0)),
            _ => |s: &BatteryState| format!("{}\n", s.voltage_min_design.unwrap_or(0)),
        };
        spec = spec.attr(attr_name, StateAttribute::new(&state, format));
    }
    let device = model.create_and_announce(spec).await?;

    hel::spawn(crate::log_subsystem_errors(
        "power_supply-monitor",
        monitor(hw, state, device.downgrade()),
    ));
    Ok(vec![Box::new(device)])
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    // The class directory exists even before the first device shows up.
    model.subsystem(&Membership::class("power_supply"))?;
    // Only batteries are supported.
    let filters = [
        mbus::Filter::Equals("class", "power_supply"),
        mbus::Filter::Equals("power_supply.type", "battery"),
    ];
    observe(mbus::Filter::Conjunction(&filters), |scope, event| {
        let what = format!(
            "power supply {}",
            string_prop(event.properties(), "power_supply.id").unwrap_or("?")
        );
        scope.install(what, install_entity(model.clone(), event));
    })
    .await
}
