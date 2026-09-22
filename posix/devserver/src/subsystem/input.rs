//! The input class subsystem: evdev devices with their capability attributes.

use std::sync::Arc;

use anyhow::{Result, bail};
use async_trait::async_trait;
use hel::Handle;
use managarm::fs;
use managarm::mbus;

use crate::device::{
    AttrGroup, DevNode, DevNodeSpec, DeviceKey, DeviceSpec, IdAllocator, Membership, Model,
    NodeType, Parent, Role,
};
use crate::subsystem::{Installed, device_parent_ref, observe, remote_lane};
use crate::sysfs::Attribute;

const INPUT_MAJOR: i32 = 13;

const EV_KEY: i32 = 0x01;
const EV_REL: i32 = 0x02;
const EV_ABS: i32 = 0x03;

const EV_MAX: usize = 0x1f;
const KEY_MAX: usize = 0x2ff;
const REL_MAX: usize = 0x0f;
const ABS_MAX: usize = 0x3f;

fn eviocgbit(ev: u64) -> u64 {
    // _IOC(_IOC_READ, 'E', 0x20 + ev, 0)
    (2 << 30) | (('E' as u64) << 8) | (0x20 + ev)
}

/// A capabilities/* attribute; every read issues an EVIOCGBIT ioctl to the
/// driver, like the evdev capability files on Linux.
struct CapabilityAttribute {
    lane: Arc<Handle>,
    input_type: Option<i32>,
    words: usize,
}

impl CapabilityAttribute {
    fn new(lane: Arc<Handle>, input_type: Option<i32>, max_bit: usize) -> Arc<Self> {
        Arc::new(Self {
            lane,
            input_type,
            words: (max_bit + 64) / 64,
        })
    }

    async fn read_bits(&self) -> Result<Vec<u8>> {
        let file = fs::client::open_device(&self.lane).await?;
        let mut buffer = vec![0u8; self.words * 8];

        let mut req = fs::bindings::GenericIoctlRequest::new();
        req.set_command(eviocgbit(if self.input_type.is_some() { 1 } else { 0 }));
        if let Some(input_type) = self.input_type {
            req.set_input_type(input_type);
        }
        req.set_size(buffer.len() as i32);

        file.ioctl(&req, async |conversation| {
            let (recv_resp, recv_data) = hel::submit_async(
                &conversation,
                (hel::ReceiveInline, hel::ReceiveBuffer::new(&mut buffer)),
            )
            .await?;

            let resp: fs::bindings::GenericIoctlReply = bragi::head_from_bytes(&recv_resp?)?;
            if resp.error() != fs::bindings::Errors::Success {
                bail!("EVIOCGBIT failed: {:?}", resp.error());
            }
            // The driver only sends the data if the ioctl succeeded.
            recv_data?;
            Ok(())
        })
        .await?;

        // Space-separated hex words, most significant first, leading zero
        // words suppressed; this matches both posix and Linux.
        let words: Vec<u64> = buffer
            .chunks_exact(8)
            .map(|chunk| u64::from_le_bytes(chunk.try_into().unwrap()))
            .collect();
        let mut out = String::new();
        let mut suffix = false;
        for (i, word) in words.iter().rev().enumerate() {
            if *word == 0 && !suffix {
                continue;
            }
            if i != 0 {
                out.push(' ');
            }
            out += &format!("{word:x}");
            suffix = true;
        }
        Ok(out.into_bytes())
    }
}

#[async_trait(?Send)]
impl Attribute for CapabilityAttribute {
    async fn show(&self) -> Result<Vec<u8>, fs::server::Error> {
        self.read_bits().await.map_err(|e| {
            eprintln!("devserver: reading input capabilities failed: {e:#}");
            fs::server::Error::InternalError
        })
    }
}

async fn install_entity(
    model: Arc<Model>,
    event: mbus::EnumerationEvent,
    index: i32,
) -> Result<Vec<Box<dyn Installed>>> {
    println!("devserver: Installing input device input/event{index}");
    let lane = Arc::new(remote_lane(&event).await?);

    let input_spec = DeviceSpec::new(
        format!("input{index}"),
        device_parent_ref(event.properties()),
        Membership::class("input"),
    )
    .key(DeviceKey::primary(event.entity_id()));
    let input_device = model.create_and_announce(input_spec).await?;

    // The capability attributes exist before the add uevent: udev's input_id
    // builtin reads them right after the uevent.
    let event_name = format!("event{index}");
    let event_spec = DeviceSpec::new(
        event_name.clone(),
        Parent::Device(input_device.device().clone()),
        Membership::class("input"),
    )
    .key(DeviceKey::new(event.entity_id(), Role::InputEvent))
    .devnode(DevNodeSpec::Managed(
        DevNode {
            path: format!("input/{event_name}"),
            node_type: NodeType::Char,
            major: INPUT_MAJOR,
            // evdev devices start at minor 64.
            minor: 64 + index,
        },
        lane.clone(),
    ))
    .group(
        AttrGroup::named("capabilities")
            .attr("ev", CapabilityAttribute::new(lane.clone(), None, EV_MAX))
            .attr(
                "key",
                CapabilityAttribute::new(lane.clone(), Some(EV_KEY), KEY_MAX),
            )
            .attr(
                "rel",
                CapabilityAttribute::new(lane.clone(), Some(EV_REL), REL_MAX),
            )
            .attr(
                "abs",
                CapabilityAttribute::new(lane.clone(), Some(EV_ABS), ABS_MAX),
            ),
    );
    let event_device = model.create_and_announce(event_spec).await?;
    Ok(vec![Box::new(input_device), Box::new(event_device)])
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    // The class directory exists even before the first device shows up.
    model.subsystem(&Membership::class("input"))?;
    let mut indices = IdAllocator::new(0);

    observe(
        mbus::Filter::Equals("unix.subsystem", "input"),
        |scope, event| {
            let index = indices.allocate() as i32;
            scope.install(
                format!("input device event{index}"),
                install_entity(model.clone(), event, index),
            );
        },
    )
    .await
}
