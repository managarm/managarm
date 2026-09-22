//! The usbmisc class subsystem: USB devices without a more specific class
//! (CDC WDM control channels). The generic subsystem owns their /dev node;
//! this subsystem only gives it a sysfs identity.

use std::sync::Arc;

use anyhow::{Result, bail};
use managarm::mbus;

use crate::device::{DevNodeSpec, DeviceKey, DeviceSpec, Membership, Model};
use crate::subsystem::generic::GenericDevice;
use crate::subsystem::{Installed, device_parent_ref, observe, parse_prop};

async fn install_entity(
    model: Arc<Model>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let node_id: i64 = parse_prop(properties, "usbmisc.node-entity")?;

    // The generic subsystem names the node after its own allocation order.
    let generic = model.wait_device(&DeviceKey::primary(node_id)).await;
    let node = match (generic.data::<GenericDevice>(), generic.devnode()) {
        (Some(_), Some(node)) => node.clone(),
        _ => bail!("{} is not a generic device", generic.dir().sysfs_path()),
    };
    let spec = DeviceSpec::new(
        node.path.clone(),
        device_parent_ref(properties),
        Membership::class("usbmisc"),
    )
    .key(DeviceKey::primary(event.entity_id()))
    .devnode(DevNodeSpec::Foreign(node));
    Ok(vec![Box::new(model.create_and_announce(spec).await?)])
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    // The class directory exists even before the first device shows up.
    model.subsystem(&Membership::class("usbmisc"))?;
    observe(
        mbus::Filter::Equals("unix.subsystem", "usbmisc"),
        |scope, event| {
            scope.install(
                format!("usbmisc device {}", event.entity_id()),
                install_entity(model.clone(), event),
            );
        },
    )
    .await
}
