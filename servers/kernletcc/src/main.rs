//! kernletcc: compiles Fafnir IR sent by drivers into in-kernel kernlets.
//!
//! Registers a `kernletcc` mbus entity, accepts `CompileRequest`s,
//! compiles the Fafnir bytecode into an ELF DSO and uploads it to `kernletctl`.

mod elf;
mod fafnir;

use std::rc::Rc;

use anyhow::{Result, bail, ensure};
use fafnir::BindType;
use hel::Handle;
use managarm::mbus;

bragi::include_binding!(mod kernlet = "kernlet.rs");

fn bind_type_of(proto: kernlet::ParameterType) -> BindType {
    match proto {
        kernlet::ParameterType::Offset => BindType::Offset,
        kernlet::ParameterType::MemoryView => BindType::MemoryView,
        kernlet::ParameterType::BitsetEvent => BindType::BitsetEvent,
    }
}

/// Finds the kernel's `kernletctl` entity and returns its remote lane.
async fn enumerate_ctl() -> Result<Handle> {
    let mut enumerator = mbus::Enumerator::new(mbus::Filter::Equals("class", "kernletctl"));
    loop {
        let (_overflow, events) = enumerator.next_events().await?;
        if let Some(event) = events.into_iter().next() {
            return Ok(event.entity().get_remote_lane().await?);
        }
    }
}

/// Uploads a compiled ELF DSO to kernletctl and returns the resulting kernlet descriptor.
async fn upload(
    ctl: &Handle,
    elf_bytes: &[u8],
    bind_types: &[kernlet::ParameterType],
) -> Result<Handle> {
    let req = kernlet::UploadRequest::new(bind_types.to_vec());
    let (head, tail) = bragi::head_tail_to_bytes(&req)?;

    let (_offer, (_send_head, _send_tail, _send_elf, recv_resp, pull)) = hel::submit_async(
        ctl,
        hel::Offer::new((
            hel::SendBuffer::new(&head),
            hel::SendBuffer::new(&tail),
            hel::SendBuffer::new(elf_bytes),
            hel::ReceiveInline,
            hel::PullDescriptor,
        )),
    )
    .await?;

    let resp_data = recv_resp?;
    let resp: kernlet::SvrResponse = bragi::head_from_bytes(&resp_data)?;
    ensure!(
        resp.error() == kernlet::Error::Success,
        "kernletctl rejected the upload"
    );

    Ok(pull?.expect("kernletctl did not push a kernlet descriptor"))
}

/// Handles a single request on the given lane. Returns `false` if the lane was shut down.
async fn handle_request(lane: &Handle, ctl: &Handle) -> Result<bool> {
    let (conv, (head,)) = hel::submit_async(lane, hel::Accept::new((hel::ReceiveInline,))).await?;

    let conversation = match conv {
        Ok(Some(lane)) => lane,
        Ok(None) => bail!("accept did not yield a conversation lane"),
        Err(hel::Error::EndOfLane) | Err(hel::Error::LaneShutdown) => return Ok(false),
        Err(e) => return Err(e.into()),
    };
    let head = head?;

    // Receive the tail and the Fafnir bytecode on the conversation lane.
    let preamble = bragi::preamble_from_bytes(&head)?;
    let mut tail = vec![0u8; preamble.tail_size() as usize];
    let (recv_tail, recv_code) = hel::submit_async(
        &conversation,
        (hel::ReceiveBuffer::new(&mut tail), hel::ReceiveInline),
    )
    .await?;
    recv_tail?;
    let code = recv_code?;

    let req: kernlet::CompileRequest = bragi::head_tail_from_bytes(&head, &tail)?;
    let proto: Vec<kernlet::ParameterType> = req.bind_types().to_vec();
    let bind_types: Vec<BindType> = proto.iter().map(|&p| bind_type_of(p)).collect();

    let compiled = fafnir::compile(&code, &bind_types)?;
    let elf_bytes = elf::build_dso(&compiled)?;
    let kernlet = upload(ctl, &elf_bytes, &proto).await?;

    let resp = kernlet::SvrResponse::new(kernlet::Error::Success);
    let resp_head = bragi::head_to_bytes(&resp)?;
    let (send_resp, push) = hel::submit_async(
        &conversation,
        (
            hel::SendBuffer::new(&resp_head),
            hel::PushDescriptor::new(&kernlet),
        ),
    )
    .await?;
    send_resp?;
    push?;

    Ok(true)
}

/// Serves compilation requests on a single client lane until it is shut down.
async fn serve_compiler(lane: Handle, ctl: Rc<Handle>) {
    loop {
        match handle_request(&lane, &ctl).await {
            Ok(true) => {}
            Ok(false) => return,
            Err(e) => {
                eprintln!("kernletcc: error while serving a request: {e:?}");
                return;
            }
        }
    }
}

async fn run() -> Result<()> {
    let ctl = Rc::new(enumerate_ctl().await?);

    let mut properties = mbus::Properties::new();
    properties.insert(
        "class".to_string(),
        mbus::Item::String("kernletcc".to_string()),
    );
    let entity = mbus::create_entity("kernletcc", &properties).await?;

    loop {
        let (local, remote) = hel::create_stream()?;
        entity.serve_remote_lane(remote).await?;

        let ctl = ctl.clone();
        hel::spawn(async move {
            serve_compiler(local, ctl).await;
        });
    }
}

fn main() -> Result<()> {
    println!("kernletcc: Starting up");
    hel::block_on(run())?
}
