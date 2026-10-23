use anyhow::{Context, Result, bail};
use async_lock::RwLock;
use bragi::Message;
use hel::{Handle, SwapSpace, submission::result::ManageRequest};
use managarm::mbus;

bragi::include_binding!(mod proto = "swap.rs");

const PAGE_SHIFT: usize = 12;
const PAGE_SIZE: usize = 1 << PAGE_SHIFT;

const MAX_REGION_SIZE: usize = (1 << hel_sys::kHelSwapRegionShift as usize) - PAGE_SIZE;
const OFFSET_MASK: usize = (1 << hel_sys::kHelSwapRegionShift as usize) - 1;

struct State {
    swap_space: SwapSpace,
    swap_targets: RwLock<Vec<Handle>>,
}

async fn serve_client(state: &State, lane: Handle) {
    loop {
        match handle_client_request(state, &lane).await {
            Ok(true) => {}
            Ok(false) => return,
            Err(err) => {
                eprintln!("swapserver: Error while serving a request: {err:?}");
                return;
            }
        }
    }
}

async fn handle_client_request(state: &State, lane: &Handle) -> Result<bool> {
    let (conv, (head,)) = hel::submit_async(lane, hel::Accept::new((hel::ReceiveInline,))).await?;

    let conversation = match conv {
        Ok(Some(lane)) => lane,
        Ok(None) => bail!("accept did not yield a conversation lane"),
        Err(hel::Error::EndOfLane) | Err(hel::Error::LaneShutdown) => return Ok(false),
        Err(e) => return Err(e.into()),
    };
    let head = head?;
    let preamble = bragi::preamble_from_bytes(&head)?;

    match preamble.id() {
        proto::GetSwapSpaceRequest::MESSAGE_ID => {
            handle_get_swap_space(state, &conversation).await?;
        }
        id => {
            eprintln!("swapserver: Dismissing request with unexpected message ID {id}.");
            hel::submit_async(&conversation, hel::Dismiss).await??;
        }
    }

    Ok(true)
}

async fn handle_get_swap_space(state: &State, conversation: &Handle) -> Result<()> {
    let resp = proto::GetSwapSpaceResponse::new(proto::Errors::Success);
    let resp_head = bragi::head_to_bytes(&resp)?;
    let (send_head, push_handle) = hel::submit_async(
        conversation,
        (
            hel::SendBuffer::new(&resp_head),
            hel::PushDescriptor::new(state.swap_space.swap_handle(), hel_sys::kHelRightAssign),
        ),
    )
    .await?;
    send_head?;
    push_handle?;
    Ok(())
}

async fn manage_request_loop(state: &'static State) {
    loop {
        let manage_request = hel::submission::manage_memory(state.swap_space.backing_handle())
            .await
            .unwrap();

        hel::spawn(async move {
            if let Err(e) = handle_manage_request(state, manage_request).await {
                eprintln!("swapserver: Failed to serve manage request: {e:?}");
            }
        });
    }
}

/// Watches mbus for swap targets and starts swapping to each one that shows up.
async fn discover_swap_targets(state: &'static State) {
    let mut enumerator = mbus::Enumerator::new(mbus::Filter::Equals("class", "swap-target"));

    loop {
        let (_, events) = enumerator.next_events().await.unwrap();

        for event in events {
            if event.event_type() != mbus::EventType::Created {
                continue;
            }

            hel::spawn(async move {
                if let Err(e) = add_swap_target(state, &event).await {
                    eprintln!("swapserver: Ignoring swap target {}: {e:?}", event.name());
                }
            });
        }
    }
}

async fn handle_manage_request(state: &State, manage_request: ManageRequest) -> Result<()> {
    let region = manage_request.offset() >> hel_sys::kHelSwapRegionShift as usize;
    let offset = manage_request.offset() & OFFSET_MASK;

    let swap_targets = state.swap_targets.read().await;
    let lane = &swap_targets[region];

    let request = proto::ManageRequest::new(
        match manage_request {
            ManageRequest::Initialize { .. } => proto::ManageType::Initialize,
            ManageRequest::Writeback { .. } => proto::ManageType::Writeback,
        },
        (offset >> PAGE_SHIFT) as u64,
        (manage_request.length() >> PAGE_SHIFT) as u64,
    );

    let request_head = bragi::head_to_bytes(&request)?;
    let (offer, send_head) = hel::submit_async(
        lane,
        hel::Offer::new_with_lane(hel::SendBuffer::new(&request_head)),
    )
    .await?;
    send_head?;
    let conversation = offer?.context("offer did not yield a conversation lane")?;

    let resp_head = hel::submit_async(&conversation, hel::ReceiveInline).await??;
    let resp: proto::ManageResponse = bragi::head_from_bytes(&resp_head)?;

    if resp.error() != proto::Errors::Success {
        bail!("swap target failed to service a manage request");
    }

    hel::complete_manage_request(state.swap_space.backing_handle(), manage_request)
        .context("failed to complete a manage request")?;

    Ok(())
}

/// Validates a newly created swap target and spawns a task that serves it.
async fn add_swap_target(state: &State, event: &mbus::EnumerationEvent) -> Result<()> {
    let size = event
        .properties()
        .iter()
        .find(|(name, _)| *name == "size")
        .context("missing size property")
        .and_then(|(_, value)| -> Result<usize> {
            match value {
                mbus::Item::String(value) => value.parse().context("invalid size property"),
                mbus::Item::Array(_) => bail!("unexpected array item for size property"),
            }
        })?;
    if size < PAGE_SIZE || size > MAX_REGION_SIZE {
        bail!(
            "swap target {} has an invalid size: {size} bytes",
            event.name()
        );
    }

    let lane = event
        .entity()
        .get_remote_lane()
        .await
        .context("failed to connect to swap target")?;
    println!(
        "swapserver: Found a swap target {} with size of {size} bytes",
        event.name()
    );

    // Only enable writeback once the target is able to service manage requests.
    // The last page of a region always stays unused.
    let region_pages = size >> PAGE_SHIFT;

    let mut swap_targets = state.swap_targets.write().await;

    let region = swap_targets.len();
    let view = hel::create_slice_view(
        state.swap_space.backing_handle(),
        region << hel_sys::kHelSwapRegionShift as usize,
        region_pages << PAGE_SHIFT,
    )
    .context("failed to create a slice of backing memory")?;

    hel::submit_async(
        &lane,
        hel::PushDescriptor::new(
            &view,
            hel_sys::kHelRightRead
                | hel_sys::kHelRightWrite
                | hel_sys::kHelRightAssign
                | hel_sys::kHelRightManage,
        ),
    )
    .await??;

    hel::submission::set_swap_region(state.swap_space.swap_handle(), region as u32, region_pages)
        .await
        .context("failed to enable swap region")?;

    swap_targets.push(lane);

    Ok(())
}

async fn run(state: &'static State) -> Result<()> {
    let mut properties = mbus::Properties::new();
    properties.insert(
        "class".to_string(),
        mbus::Item::String("swapserver".to_string()),
    );
    let entity = mbus::create_entity("swapserver", &properties).await?;

    hel::spawn(manage_request_loop(state));
    hel::spawn(discover_swap_targets(state));

    loop {
        let (local, remote) = hel::create_stream()?;
        entity.serve_remote_lane(remote).await?;

        hel::spawn(serve_client(state, local));
    }
}

fn main() -> Result<()> {
    // The state lives for as long as the server does.
    let swap_space = hel::create_swap_space(managarm::posix::hierarchy_handle())?;
    let state: &'static State = Box::leak(Box::new(State {
        swap_space,
        swap_targets: RwLock::new(Vec::with_capacity(8)),
    }));

    println!("swapserver: Starting up");
    hel::block_on(run(state))?
}
