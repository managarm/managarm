//! /sys/firmware/devicetree: device tree nodes with their properties.
//!
//! Nodes are devices (they emit uevents that carry the compatible strings),
//! but they live under /sys/firmware/devicetree/base rather than /sys/devices.

use std::sync::Arc;

use anyhow::Result;
use managarm::hw;
use managarm::mbus;

use crate::device::{DeviceKey, DeviceSpec, Membership, Model, Parent, Placement};
use crate::subsystem::{Installed, mbus_parent, observe, remote_lane};
use crate::sysfs::{StaticAttribute, SysfsNode};

fn string_list(data: &[u8]) -> Vec<String> {
    data.split(|byte| *byte == 0)
        .filter(|s| !s.is_empty())
        .map(|s| String::from_utf8_lossy(s).into_owned())
        .collect()
}

async fn install_node(
    model: Arc<Model>,
    base: Arc<SysfsNode>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let hw = hw::Device::new(remote_lane(&event).await?);
    let path = hw.get_dt_path().await?;
    println!(
        "devserver: Installing DT device {path} (mbus ID: {})",
        event.entity_id()
    );

    let slash = path.rfind('/').unwrap_or(0);
    let name = path[slash + 1..].to_string();
    // Children of the root node have no parent device; they go into base/.
    let (parent, placement) = match mbus_parent(event.properties()) {
        Some(parent) if slash != 0 => (Parent::Key(DeviceKey::primary(parent)), Placement::Default),
        _ => (Parent::None, Placement::Under(base)),
    };
    let mut spec = DeviceSpec::new(name, parent, Membership::None)
        .key(DeviceKey::primary(event.entity_id()))
        .placement(placement);

    let properties = hw.get_dt_properties().await?;
    let compatible = properties
        .iter()
        .find(|property| property.name == "compatible")
        .map(|property| string_list(&property.data))
        .unwrap_or_default();
    for (i, value) in compatible.iter().enumerate() {
        spec = spec.uevent_extra(&format!("OF_COMPATIBLE_{i}"), value.clone());
    }
    spec = spec.uevent_extra("OF_COMPATIBLE_N", compatible.len().to_string());

    for property in properties {
        spec = spec.attr(&property.name, StaticAttribute::new_sized(property.data));
    }
    Ok(vec![Box::new(model.create_and_announce(spec).await?)])
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    let base = model.firmware_dir.dir("devicetree")?.dir("base")?;

    observe(
        mbus::Filter::Equals("unix.subsystem", "dt"),
        |scope, event| {
            scope.install(
                format!("DT node {}", event.entity_id()),
                install_node(model.clone(), base.clone(), event),
            );
        },
    )
    .await
}
