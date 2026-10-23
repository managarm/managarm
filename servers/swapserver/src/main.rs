use anyhow::{Context, Result, bail};
use bragi::Message;
use hel::{Handle, SwapSpace, submission::result::ManageRequest};
use managarm::mbus;

bragi::include_binding!(mod proto = "swap.rs");

const PAGE_SHIFT: usize = 12;
const PAGE_SIZE: usize = 1 << PAGE_SHIFT;

async fn serve_client(swap_space: &SwapSpace, lane: Handle) {
    loop {
        match handle_client_request(swap_space, &lane).await {
            Ok(true) => {}
            Ok(false) => return,
            Err(err) => {
                eprintln!("swapserver: Error while serving a request: {err:?}");
                return;
            }
        }
    }
}

async fn handle_client_request(swap_space: &SwapSpace, lane: &Handle) -> Result<bool> {
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
            handle_get_swap_space(swap_space, &conversation).await?;
        }
        id => {
            eprintln!("swapserver: Dismissing request with unexpected message ID {id}.");
            hel::submit_async(&conversation, hel::Dismiss).await??;
        }
    }

    Ok(true)
}

async fn handle_get_swap_space(swap_space: &SwapSpace, conversation: &Handle) -> Result<()> {
    let resp = proto::GetSwapSpaceResponse::new(proto::Errors::Success);
    let resp_head = bragi::head_to_bytes(&resp)?;
    let (send_head, push_handle) = hel::submit_async(
        conversation,
        (
            hel::SendBuffer::new(&resp_head),
            hel::PushDescriptor::new(swap_space.swap_handle(), hel_sys::kHelRightAssign),
        ),
    )
    .await?;
    send_head?;
    push_handle?;
    Ok(())
}

/// Waits for the first swap target to show up on mbus and starts swapping to it.
async fn discover_swap_target(swap_space: &'static SwapSpace) -> Result<()> {
    let mut enumerator = mbus::Enumerator::new(mbus::Filter::Equals("class", "swap-target"));
    let event = loop {
        let (_, events) = enumerator
            .next_events()
            .await
            .context("mbus enumeration failed")?;
        if let Some(event) = events
            .into_iter()
            .find(|event| event.event_type() == mbus::EventType::Created)
        {
            break event;
        }
    };

    let size = event
        .properties()
        .iter()
        .find(|(name, _)| *name == "size")
        .context("missing size property")
        .and_then(|(_, value)| -> Result<usize> {
            match value {
                mbus::Item::String(value) => Ok(value.parse()?),
                mbus::Item::Array(_) => bail!("unexpected array item"),
            }
        })?;
    println!("swapserver: Found a swap target of {size} bytes");

    let lane = event.entity().get_remote_lane().await?;
    hel::spawn(async move {
        if let Err(e) = serve_swap_target(swap_space, lane, size).await {
            eprintln!("swapserver: Failed to serve swap target: {e:?}");
        }
    });

    Ok(())
}

async fn serve_swap_target(swap_space: &SwapSpace, lane: Handle, size: usize) -> Result<()> {
    let backing = swap_space.backing_handle();

    hel::submit_async(
        &lane,
        hel::PushDescriptor::new(
            backing,
            hel_sys::kHelRightRead
                | hel_sys::kHelRightWrite
                | hel_sys::kHelRightAssign
                | hel_sys::kHelRightManage,
        ),
    )
    .await??;

    // Only enable writeback once the target is able to service manage requests.
    // The target backs region 0, whose offsets start at zero.
    // The last page of a region always stays unused.
    let max_region_pages = (1 << (hel_sys::kHelSwapRegionShift as usize - PAGE_SHIFT)) - 1;
    hel::submission::set_swap_region(
        swap_space.swap_handle(),
        0,
        (size / PAGE_SIZE).min(max_region_pages),
    )
    .await
    .context("failed to attach swap region")?;

    loop {
        let manage_request = hel::submission::manage_memory(backing).await?;
        let request = proto::ManageRequest::new(
            match manage_request {
                ManageRequest::Initialize { .. } => proto::ManageType::Initialize,
                ManageRequest::Writeback { .. } => proto::ManageType::Writeback,
            },
            (manage_request.offset() / PAGE_SIZE) as u64,
            (manage_request.length() / PAGE_SIZE) as u64,
        );
        let request_head = bragi::head_to_bytes(&request)?;
        let (offer, send_head) = hel::submit_async(
            &lane,
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

        hel::complete_manage_request(backing, manage_request)
            .context("failed to complete a manage request")?;
    }
}

async fn run(swap_space: &'static SwapSpace) -> Result<()> {
    let mut properties = mbus::Properties::new();
    properties.insert(
        "class".to_string(),
        mbus::Item::String("swapserver".to_string()),
    );
    let entity = mbus::create_entity("swapserver", &properties).await?;

    hel::spawn(async move {
        if let Err(e) = discover_swap_target(swap_space).await {
            eprintln!("swapserver: Failed to discover a swap target: {e:?}");
        }
    });

    loop {
        let (local, remote) = hel::create_stream()?;
        entity.serve_remote_lane(remote).await?;

        hel::spawn(serve_client(swap_space, local));
    }
}

fn main() -> Result<()> {
    // The swap space lives for as long as the server does.
    let swap_space: &'static SwapSpace = Box::leak(Box::new(hel::create_swap_space(
        managarm::posix::hierarchy_handle(),
    )?));

    println!("swapserver: Starting up");
    hel::block_on(run(swap_space))?
}
