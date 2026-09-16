//! The sound class subsystem: cards with their control and PCM devices.
//!
//! A card's control device is announced only once all of the card's PCM
//! devices exist, since ALSA enumerates PCM devices through the control node.

use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex};

use anyhow::{Context, Result, bail};
use managarm::mbus;

use crate::EXPECT_LOCK;
use crate::device::{
    DevNode, DevNodeSpec, DeviceKey, DeviceSpec, IdAllocator, Membership, Model, NodeType, Parent,
    Role,
};
use crate::subsystem::{
    Installed, mbus_parent, observe, parse_prop, remote_lane, required_prop, string_prop,
};

const SOUND_MAJOR: i32 = 116;

/// The data of a card device.
struct Card {
    index: u32,
    remaining_children: AtomicU64,
    pcm_ids: Mutex<IdAllocator>,
}

/// Counts a PCM device as done once its installation ends, whether it
/// succeeded or not; one failed PCM must not keep the control device of its
/// card unannounced.
struct PcmDone {
    model: Arc<Model>,
    card: Arc<Card>,
}

impl Drop for PcmDone {
    fn drop(&mut self) {
        let _ =
            self.card
                .remaining_children
                .fetch_update(Ordering::AcqRel, Ordering::Acquire, |n| {
                    Some(n.saturating_sub(1))
                });
        self.model.update.notify(usize::MAX);
    }
}

struct State {
    model: Arc<Model>,
    card_ids: Mutex<IdAllocator>,
    minors: Mutex<IdAllocator>,
}

async fn install_card(
    state: Arc<State>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let card_id = event.entity_id();
    let num_devices: u64 = parse_prop(properties, "sound.num-devices")?;
    let index = state.card_ids.lock().expect(EXPECT_LOCK).allocate();
    println!("devserver: Installing sound card snd/card{index}");

    let lane = Arc::new(remote_lane(&event).await?);
    let card_device = state
        .model
        .create(
            DeviceSpec::new(
                format!("card{index}"),
                Parent::None,
                Membership::class("sound"),
            )
            .key(DeviceKey::primary(card_id))
            .data(Card {
                index,
                remaining_children: AtomicU64::new(num_devices),
                pcm_ids: Mutex::new(IdAllocator::new(0)),
            }),
        )
        .await?;

    let minor = state.minors.lock().expect(EXPECT_LOCK).allocate() as i32;
    let control_spec = DeviceSpec::new(
        format!("controlC{index}"),
        Parent::Device(card_device.device().clone()),
        Membership::class("sound"),
    )
    .key(DeviceKey::new(card_id, Role::SoundControl))
    .devnode(DevNodeSpec::Managed(
        DevNode {
            path: format!("snd/controlC{index}"),
            node_type: NodeType::Char,
            major: SOUND_MAJOR,
            minor,
        },
        lane,
    ));
    let control = state.model.create(control_spec).await?;
    let card_device = state.model.announce(card_device).await?;

    let card = card_device
        .data::<Card>()
        .expect("the card device carries its card");
    state
        .model
        .wait_until(|| card.remaining_children.load(Ordering::Acquire) == 0)
        .await;
    let control = state.model.announce(control).await?;
    Ok(vec![Box::new(card_device), Box::new(control)])
}

async fn install_pcm(
    state: Arc<State>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let Some(card_id) = mbus_parent(properties) else {
        bail!("sound device has no card");
    };
    let card_device = state.model.wait_device(&DeviceKey::primary(card_id)).await;
    let card = card_device
        .data::<Card>()
        .with_context(|| format!("{} is not a sound card", card_device.dir().sysfs_path()))?;
    let _done = PcmDone {
        model: state.model.clone(),
        card: card.clone(),
    };

    let playback = match required_prop(properties, "sound.type")? {
        "playback" => true,
        "capture" => false,
        other => bail!("unsupported sound device type '{other}'"),
    };

    let device_index = card.pcm_ids.lock().expect(EXPECT_LOCK).allocate();
    let minor = state.minors.lock().expect(EXPECT_LOCK).allocate() as i32;
    let name = format!(
        "pcmC{}D{device_index}{}",
        card.index,
        if playback { 'p' } else { 'c' }
    );
    let lane = Arc::new(remote_lane(&event).await?);
    let spec = DeviceSpec::new(
        name.clone(),
        Parent::Device(card_device),
        Membership::class("sound"),
    )
    .key(DeviceKey::primary(event.entity_id()))
    .devnode(DevNodeSpec::Managed(
        DevNode {
            path: format!("snd/{name}"),
            node_type: NodeType::Char,
            major: SOUND_MAJOR,
            minor,
        },
        lane,
    ));
    Ok(vec![Box::new(state.model.create_and_announce(spec).await?)])
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    // The class directory exists even before the first device shows up.
    model.subsystem(&Membership::class("sound"))?;
    let state = Arc::new(State {
        model,
        card_ids: Mutex::new(IdAllocator::new(0)),
        minors: Mutex::new(IdAllocator::new(0)),
    });

    let filters = [
        mbus::Filter::Equals("class", "sound-card"),
        mbus::Filter::Equals("class", "sound-device"),
    ];
    observe(mbus::Filter::Disjunction(&filters), |scope, event| {
        let what = format!("sound entity {}", event.entity_id());
        let class = string_prop(event.properties(), "class")
            .unwrap_or("")
            .to_string();
        match class.as_str() {
            "sound-card" => scope.install(what, install_card(state.clone(), event)),
            "sound-device" => scope.install(what, install_pcm(state.clone(), event)),
            other => println!(
                "devserver: unsupported sound device class '{other}' (mbus ID {})",
                event.entity_id()
            ),
        }
    })
    .await
}
