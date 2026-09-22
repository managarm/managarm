//! The drm class subsystem: card and render nodes.

use std::sync::Arc;

use anyhow::Result;
use hel::Handle;
use managarm::mbus;

use crate::device::{
    AnnouncedDevice, DevNode, DevNodeSpec, DeviceKey, DeviceSpec, IdAllocator, Membership, Model,
    NodeType, Role,
};
use crate::subsystem::{Installed, attr, observe, parent_ref, remote_lane};

const DRM_MAJOR: i32 = 226;

async fn install_node(
    model: &Arc<Model>,
    event: &mbus::EnumerationEvent,
    name: String,
    minor: i32,
    role: Role,
    lane: Arc<Handle>,
) -> Result<AnnouncedDevice> {
    let spec = DeviceSpec::new(
        name.clone(),
        parent_ref(event.properties()),
        Membership::class("drm"),
    )
    .key(DeviceKey::new(event.entity_id(), role))
    .devtype("drm_minor")
    .devnode(DevNodeSpec::Managed(
        DevNode {
            path: format!("dri/{name}"),
            node_type: NodeType::Char,
            major: DRM_MAJOR,
            minor,
        },
        lane,
    ))
    .attrs(vec![attr("dev", format!("{DRM_MAJOR}:{minor}\n"))]);
    model.create_and_announce(spec).await
}

async fn install_entity(
    model: Arc<Model>,
    event: mbus::EnumerationEvent,
    index: i32,
) -> Result<Vec<Box<dyn Installed>>> {
    println!("devserver: Installing DRM device card{index}");
    let lane = Arc::new(remote_lane(&event).await?);
    let card = install_node(
        &model,
        &event,
        format!("card{index}"),
        index,
        Role::Primary,
        lane.clone(),
    )
    .await?;
    let render = format!("renderD{}", index + 128);
    let render = install_node(&model, &event, render, index + 128, Role::DrmRender, lane).await?;
    Ok(vec![Box::new(card), Box::new(render)])
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    // The class directory exists even before the first device shows up.
    model.subsystem(&Membership::class("drm"))?;
    let mut indices = IdAllocator::new(0);

    observe(
        mbus::Filter::Equals("unix.subsystem", "drm"),
        |scope, event| {
            let index = indices.allocate() as i32;
            scope.install(
                format!("DRM device card{index}"),
                install_entity(model.clone(), event, index),
            );
        },
    )
    .await
}
