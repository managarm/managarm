//! Client side of the fs protocol: opening devices served by drivers and
//! issuing ioctls on them.

use anyhow::{Context, Result, bail};

use bragi::Message;
use hel::Handle;

use super::bindings;

/// A file opened on a driver via DEV_OPEN.
pub struct DeviceFile {
    passthrough: Handle,
}

/// Opens the device that a driver serves on the given lane.
pub async fn open_device(lane: &Handle) -> Result<DeviceFile> {
    let mut req = bindings::CntRequest::new(bindings::CntReqType::DevOpen);
    req.set_flags(0);
    let head = bragi::head_to_bytes(&req)?;

    let (_offer, (send_req, recv, pull_passthrough, pull_page)) = hel::submit_async(
        lane,
        hel::Offer::new((
            hel::SendBuffer::new(&head),
            hel::ReceiveInline,
            hel::PullDescriptor::new(hel_sys::kHelRightInvoke | hel_sys::kHelRightManage),
            hel::PullDescriptor::new(hel_sys::kHelRightRead | hel_sys::kHelRightAssign),
        )),
    )
    .await?;
    send_req?;
    let recv_data = recv?;
    let passthrough = pull_passthrough?.context("no passthrough lane pushed")?;
    // The status page is only pushed by drivers that support FC_STATUS_PAGE.
    let _ = pull_page;

    let resp: bindings::SvrResponse = bragi::head_from_bytes(&recv_data)?;
    if resp.error() != bindings::Errors::Success {
        bail!("DEV_OPEN failed: {:?}", resp.error());
    }
    Ok(DeviceFile { passthrough })
}

impl DeviceFile {
    /// Issues an ioctl carrying `req` and hands the conversation lane to `f`,
    /// which drives the remainder of the exchange.
    ///
    /// Only the head of `req` is sent: drivers receive request tails from their
    /// individual ioctl handlers, so `f` has to send one if the ioctl needs it.
    pub async fn ioctl<Req: Message, T>(
        &self,
        req: &Req,
        f: impl AsyncFnOnce(Handle) -> Result<T>,
    ) -> Result<T> {
        let ioctl_head = bragi::head_to_bytes(&bindings::IoctlRequest::new())?;
        let req_head = bragi::head_to_bytes(req)?;

        let (offer, (send_ioctl, send_req)) = hel::submit_async(
            &self.passthrough,
            hel::Offer::new_with_lane((
                hel::SendBuffer::new(&ioctl_head),
                hel::SendBuffer::new(&req_head),
            )),
        )
        .await?;
        send_ioctl?;
        send_req?;
        let conversation = offer?.context("no conversation lane offered")?;

        f(conversation).await
    }
}
