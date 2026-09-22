//! The net class subsystem: network interfaces.

use std::sync::Arc;

use anyhow::Result;
use managarm::mbus;

use crate::device::{DeviceKey, DeviceSpec, Membership, Model};
use crate::subsystem::{Installed, device_parent_ref, observe, required_prop, string_prop};

async fn install_entity(
    model: Arc<Model>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let ifname = required_prop(properties, "net.ifname")?;
    let ifindex = required_prop(properties, "net.ifindex")?;

    let spec = DeviceSpec::new(
        ifname,
        device_parent_ref(properties),
        Membership::class("net"),
    )
    .key(DeviceKey::primary(event.entity_id()))
    .devtype("wwan")
    .uevent_extra("INTERFACE", ifname)
    .uevent_extra("IFINDEX", ifindex);
    Ok(vec![Box::new(model.create_and_announce(spec).await?)])
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    // The class directory exists even before the first device shows up.
    model.subsystem(&Membership::class("net"))?;
    observe(
        mbus::Filter::Equals("unix.subsystem", "net"),
        |scope, event| {
            let properties = event.properties();
            let is_empty = |name| string_prop(properties, name).is_none_or(str::is_empty);
            if is_empty("net.ifname") || is_empty("net.ifindex") {
                println!(
                    "devserver: net device {} is missing ifname or ifindex",
                    event.entity_id()
                );
                return;
            }
            scope.install(
                format!("net device {}", event.entity_id()),
                install_entity(model.clone(), event),
            );
        },
    )
    .await
}
