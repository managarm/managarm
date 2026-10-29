//! Freestanding framebuffers (e.g., the boot framebuffer that thor publishes).
//!
//! Like Linux' simple-framebuffer, these are platform devices in /sys/devices/platform.
//!
//! The udev rule that starts gfx-plainfb matches on the add uevent of these devices.

use std::sync::Arc;

use anyhow::Result;
use managarm::mbus;

use crate::device::{DeviceKey, DeviceSpec, IdAllocator, Membership, Model, Parent, Placement};
use crate::subsystem::{Installed, observe};

async fn install_entity(
    model: Arc<Model>,
    event: mbus::EnumerationEvent,
    index: u32,
) -> Result<Vec<Box<dyn Installed>>> {
    let name = format!("freestanding-fb{index}");
    println!("devserver: Installing freestanding framebuffer {name}");
    let spec = DeviceSpec::new(name, Parent::None, Membership::bus("platform"))
        .key(DeviceKey::primary(event.entity_id()))
        .placement(Placement::Under(model.devices_dir.dir("platform")?));
    Ok(vec![Box::new(model.create_and_announce(spec).await?)])
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    // The bus directory exists even before the first device shows up.
    model.subsystem(&Membership::bus("platform"))?;
    let mut indices = IdAllocator::new(0);
    let filters = [
        mbus::Filter::Equals("class", "framebuffer"),
        mbus::Filter::Equals("unix.subsystem", "graphics"),
    ];
    observe(mbus::Filter::Conjunction(&filters), |scope, event| {
        let index = indices.allocate();
        scope.install(
            format!("framebuffer {}", event.entity_id()),
            install_entity(model.clone(), event, index),
        );
    })
    .await
}
