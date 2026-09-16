//! The block class subsystem: disks and partitions.

use std::collections::HashMap;
use std::sync::{Arc, Mutex};

use anyhow::{Context, Result, bail};
use hel::Handle;
use managarm::fs;
use managarm::mbus;

use crate::EXPECT_LOCK;
use crate::device::{
    AnnouncedDevice, DevNode, DevNodeSpec, DeviceKey, DeviceSpec, IdAllocator, Membership, Model,
    NodeType,
};
use crate::subsystem::{
    Installed, attr, observe, parent_ref, parse_prop, remote_lane, required_prop, string_prop,
};
use crate::sysfs::Attribute;

// The major for SCSI devices; minors are allocated sequentially. This is not
// really correct as the minor of a partition depends on the minor of the
// whole device (see the Linux devices.txt documentation).
const BLOCK_MAJOR: i32 = 8;

// _IOR(0x12, 114, size_t)
const BLKGETSIZE64: u64 = 0x8008_1272;

/// The data of a disk device.
struct Disk {
    /// The disk name without its suffix, which partition names are built from.
    stem: String,
}

struct State {
    model: Arc<Model>,
    minors: Mutex<IdAllocator>,
    disk_ids: Mutex<HashMap<String, IdAllocator>>,
}

impl State {
    fn allocate_disk_stem(&self, prefix: &str) -> Result<String> {
        let mut ids = self.disk_ids.lock().expect(EXPECT_LOCK);
        let id = ids
            .entry(prefix.to_string())
            .or_insert_with(|| IdAllocator::new(0))
            .allocate();

        // sd-style names are lettered, all other prefixes are numbered.
        if prefix == "sd" {
            if id >= 26 {
                bail!("ran out of sd* disk names");
            }
            Ok(format!("{prefix}{}", char::from(b'a' + id as u8)))
        } else {
            Ok(format!("{prefix}{id}"))
        }
    }
}

/// Asks the driver for the device size in bytes.
async fn query_size(lane: &Handle) -> Result<u64> {
    let mut req = fs::bindings::GenericIoctlRequest::new();
    req.set_command(BLKGETSIZE64);
    let head = bragi::head_to_bytes(&req)?;

    let (_offer, (send_head, recv)) = hel::submit_async(
        lane,
        hel::Offer::new((hel::SendBuffer::new(&head), hel::ReceiveInline)),
    )
    .await?;
    send_head?;
    let resp: fs::bindings::GenericIoctlReply = bragi::head_from_bytes(&recv?)?;
    if resp.error() != fs::bindings::Errors::Success {
        bail!("BLKGETSIZE64 failed: {:?}", resp.error());
    }
    resp.size().context("BLKGETSIZE64 reply lacks the size")
}

async fn install_device(
    state: &State,
    event: &mbus::EnumerationEvent,
    name: String,
    mut attrs: Vec<(String, Arc<dyn Attribute>)>,
    disk: Option<Disk>,
) -> Result<AnnouncedDevice> {
    let properties = event.properties();
    let lane = Arc::new(remote_lane(event).await?);
    let size = query_size(&lane).await?;
    let minor = state.minors.lock().expect(EXPECT_LOCK).allocate() as i32;

    attrs.push(attr("ro", "0\n"));
    attrs.push(attr("dev", format!("{BLOCK_MAJOR}:{minor}\n")));
    attrs.push(attr("size", format!("{}\n", size / 512)));
    if string_prop(properties, "unix.is-managarm-root") == Some("1") {
        attrs.push(attr("managarm-root", "1\n"));
    }

    let mut spec = DeviceSpec::new(name.clone(), parent_ref(properties), Membership::Block)
        .key(DeviceKey::primary(event.entity_id()))
        .devnode(DevNodeSpec::Managed(
            DevNode {
                path: name,
                node_type: NodeType::Block,
                major: BLOCK_MAJOR,
                minor,
            },
            lane,
        ))
        .attrs(attrs);
    if let Some(disk) = disk {
        spec = spec.data(disk);
    }
    state.model.create_and_announce(spec).await
}

async fn install_disk(
    state: Arc<State>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let prefix = required_prop(properties, "unix.diskname-prefix")?;
    let suffix = required_prop(properties, "unix.diskname-suffix")?;
    let stem = state.allocate_disk_stem(prefix)?;
    let name = format!("{stem}{suffix}");
    println!("devserver: Installing block device {name}");

    let device = install_device(&state, &event, name, Vec::new(), Some(Disk { stem })).await?;
    Ok(vec![Box::new(device)])
}

async fn install_partition(
    state: Arc<State>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let disk_id: i64 = parse_prop(properties, "unix.diskid")?;
    let part_suffix = required_prop(properties, "unix.partname-suffix")?;
    let part_id = required_prop(properties, "unix.partid")?;

    let disk = state.model.wait_device(&DeviceKey::primary(disk_id)).await;
    let stem = disk
        .data::<Disk>()
        .with_context(|| format!("{} is not a disk", disk.dir().sysfs_path()))?
        .stem
        .clone();
    let name = format!("{stem}{part_suffix}{part_id}");
    println!("devserver: Installing block device {name}");

    let device = install_device(&state, &event, name, Vec::new(), None).await?;
    Ok(vec![Box::new(device)])
}

async fn observe_type<F, Fut>(state: Arc<State>, block_type: &'static str, install: F) -> Result<()>
where
    F: Fn(Arc<State>, mbus::EnumerationEvent) -> Fut + 'static,
    Fut: Future<Output = Result<Vec<Box<dyn Installed>>>> + 'static,
{
    let filters = [
        mbus::Filter::Equals("unix.devtype", "block"),
        mbus::Filter::Equals("unix.blocktype", block_type),
    ];
    observe(mbus::Filter::Conjunction(&filters), |scope, event| {
        scope.install(
            format!("block {block_type} {}", event.entity_id()),
            install(state.clone(), event),
        );
    })
    .await
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    // The class directory exists even before the first device shows up.
    model.subsystem(&Membership::Block)?;
    let state = Arc::new(State {
        model,
        minors: Mutex::new(IdAllocator::new(0)),
        disk_ids: Mutex::new(HashMap::new()),
    });

    hel::spawn(crate::log_subsystem_errors(
        "block-disk",
        observe_type(state.clone(), "disk", install_disk),
    ));
    observe_type(state, "partition", install_partition).await
}
