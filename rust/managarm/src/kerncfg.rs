use anyhow::{Context, anyhow, bail};

use crate::mbus;

bragi::include_binding!(mod bindings = "kerncfg.rs");

/// Fetches the kernel command line by querying the `kerncfg` mbus entity.
pub async fn get_cmdline() -> anyhow::Result<String> {
    let mut enumerator = mbus::Enumerator::new(mbus::Filter::Equals("class", "kerncfg"));
    let (_overflow, events) = enumerator
        .next_events()
        .await
        .map_err(|e| anyhow!("mbus enumeration failed: {e}"))?;
    let event = events
        .into_iter()
        .next()
        .context("no kerncfg entity found")?;
    let lane = event
        .entity()
        .get_remote_lane()
        .await
        .map_err(|e| anyhow!("failed to open kerncfg lane: {e}"))?;

    let head = bragi::head_to_bytes(&bindings::GetCmdlineRequest::new())?;
    let (offer, (_send_head, recv)) = hel::submit_async(
        &lane,
        hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
    )
    .await?;

    let recv_data = recv?;
    let conversation_lane = offer?.context("kerncfg did not offer a lane")?;

    let response: bindings::SvrResponse = bragi::head_from_bytes(&recv_data)?;
    if response.error() != bindings::Error::Success {
        bail!("kerncfg returned error {:?}", response.error());
    }

    let size = response.size().context("kerncfg response missing size")? as usize;
    let mut buffer = vec![0u8; size];
    hel::submit_async(&conversation_lane, hel::ReceiveBuffer::new(&mut buffer)).await??;

    Ok(String::from_utf8(buffer).context("kernel cmdline is not valid UTF-8")?)
}
