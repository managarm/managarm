use std::sync::{LazyLock, OnceLock};

use anyhow::{Result, bail};
use bragi::Message;
use hel::{Handle, SwapSpace};
use managarm::mbus;

bragi::include_binding!(mod proto = "swap.rs");

static SWAP_SPACE: OnceLock<SwapSpace> = OnceLock::new();

async fn serve_client(lane: Handle) {
    loop {
        match handle_client_request(&lane).await {
            Ok(true) => {}
            Ok(false) => return,
            Err(err) => {
                eprintln!("swapserver: Error while serving a request: {err:?}");
                return;
            }
        }
    }
}

async fn handle_client_request(lane: &Handle) -> Result<bool> {
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
            handle_get_swap_space(&conversation).await?;
        }
        id => {
            eprintln!("swapserver: Dismissing request with unexpected message ID {id}.");
            hel::submit_async(&conversation, hel::Dismiss).await??;
        }
    }

    Ok(true)
}

async fn handle_get_swap_space(conversation: &hel::Handle) -> Result<()> {
    let resp = proto::GetSwapSpaceResponse::new(proto::Errors::Success);
    let resp_head = bragi::head_to_bytes(&resp)?;
    let (send_head, push_handle) = hel::submit_async(
        conversation,
        (
            hel::SendBuffer::new(&resp_head),
            hel::PushDescriptor::new(
                // SAFETY: Unwrapping here is fine because we set SWAP_SPACE at the start of main.
                SWAP_SPACE.get().unwrap().swap_handle(),
                hel_sys::kHelRightAssign,
            ),
        ),
    )
    .await?;
    send_head?;
    push_handle?;
    Ok(())
}

async fn run() -> Result<()> {
    let mut properties = mbus::Properties::new();
    properties.insert(
        "class".to_string(),
        mbus::Item::String("swapserver".to_string()),
    );
    let entity = mbus::create_entity("swsapserver", &properties).await?;

    loop {
        let (local, remote) = hel::create_stream()?;
        entity.serve_remote_lane(remote).await?;

        hel::spawn(serve_client(local));
    }
}

fn main() -> Result<()> {
    SWAP_SPACE
        .set(hel::create_swap_space(managarm::posix::hierarchy_handle())?)
        .map_err(|_| ())
        .expect("swap space to not be set");

    println!("swapserver: Starting up");
    hel::block_on(run())?
}
