//! Generic character and block devices: drivers that only want a /dev node
//! (`generic.devname` plus a sequence number). Like Linux devices without a
//! bus or class, they sit in their parent's directory or in /sys/devices.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use anyhow::Result;
use managarm::mbus;

use crate::EXPECT_LOCK;
use crate::device::{
    DevNode, DevNodeSpec, DeviceKey, DeviceSpec, IdAllocator, Membership, Model, NodeType,
};
use crate::subsystem::{Installed, observe, parent_ref, remote_lane, required_prop};

const GENERIC_CHAR_MAJOR: i32 = 234;
const GENERIC_BLOCK_MAJOR: i32 = 240;

/// The data of a generic device; the usbmisc subsystem wraps such devices.
pub struct GenericDevice;

struct State {
    model: Arc<Model>,
    // Fixed majors per type; minors are allocated sequentially.
    minors: Mutex<IdAllocator>,
    name_ids: Mutex<HashMap<String, IdAllocator>>,
}

async fn install_entity(
    state: Arc<State>,
    event: mbus::EnumerationEvent,
    node_type: NodeType,
    major: i32,
) -> Result<Vec<Box<dyn Installed>>> {
    let prefix = required_prop(event.properties(), "generic.devname")?;
    let id = state
        .name_ids
        .lock()
        .expect(EXPECT_LOCK)
        .entry(prefix.to_string())
        .or_insert_with(|| IdAllocator::new(0))
        .allocate();
    let name = format!("{prefix}{id}");
    let minor = state.minors.lock().expect(EXPECT_LOCK).allocate() as i32;
    println!(
        "devserver: Installing {} device {name}",
        if node_type == NodeType::Block {
            "block"
        } else {
            "char"
        }
    );

    let lane = Arc::new(remote_lane(&event).await?);
    // TODO: Give these devices a class; a devnode without a subsystem is a
    //       combination that Linux cannot produce.
    let spec = DeviceSpec::new(
        name.clone(),
        parent_ref(event.properties()),
        Membership::None,
    )
    .key(DeviceKey::primary(event.entity_id()))
    .devnode(DevNodeSpec::Managed(
        DevNode {
            path: name,
            node_type,
            major,
            minor,
        },
        lane,
    ))
    .data(GenericDevice);
    Ok(vec![Box::new(state.model.create_and_announce(spec).await?)])
}

async fn observe_type(state: Arc<State>, node_type: NodeType, major: i32) -> Result<()> {
    let devtype = if node_type == NodeType::Block {
        "block"
    } else {
        "char"
    };
    observe(
        mbus::Filter::Equals("generic.devtype", devtype),
        |scope, event| {
            scope.install(
                format!("generic {devtype} device {}", event.entity_id()),
                install_entity(state.clone(), event, node_type, major),
            );
        },
    )
    .await
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    let state = Arc::new(State {
        model,
        minors: Mutex::new(IdAllocator::new(0)),
        name_ids: Mutex::new(HashMap::new()),
    });
    hel::spawn(crate::log_subsystem_errors(
        "generic-block",
        observe_type(state.clone(), NodeType::Block, GENERIC_BLOCK_MAJOR),
    ));
    observe_type(state, NodeType::Char, GENERIC_CHAR_MAJOR).await
}
