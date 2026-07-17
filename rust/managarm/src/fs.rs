//! Server-side implementation of the fs passthrough protocol.

use std::rc::Rc;

use async_trait::async_trait;
use bragi::Message;

bragi::include_binding!(pub mod bindings = "fs.rs");

pub use bindings::Errors as Error;

/// Credentials of the process that issued a request.
pub type Credentials = [u8; 16];

/// Failures encountered while serving a request, as opposed to the protocol-level
/// [`Error`] that a file operation reports back to the client.
#[derive(Debug, thiserror::Error)]
pub enum ServeError {
    /// The lane was shut down; serving should stop.
    #[error("lane was shut down")]
    Shutdown,
    /// A received message could not be decoded (a protocol violation).
    #[error("malformed message: {0}")]
    Malformed(std::io::Error),
    /// A response could not be encoded.
    #[error("failed to encode a response: {0}")]
    Encode(#[from] std::io::Error),
    /// A Hel transport operation failed.
    #[error("transport operation failed: {0}")]
    Transport(#[from] hel::Error),
}

/// Operations that a file served via [`serve_passthrough`] can implement.
/// Unimplemented operations fail with [`Error::IllegalOperationTarget`].
#[allow(unused_variables)]
#[async_trait(?Send)]
pub trait File {
    async fn seek_abs(&self, offset: i64) -> Result<i64, Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn seek_rel(&self, offset: i64) -> Result<i64, Error> {
        Err(Error::IllegalOperationTarget)
    }

    async fn seek_eof(&self, offset: i64) -> Result<i64, Error> {
        Err(Error::IllegalOperationTarget)
    }

    /// Reads at most `buffer.len()` bytes into `buffer` and returns the
    /// number of bytes read.
    async fn read(&self, credentials: Credentials, buffer: &mut [u8]) -> Result<usize, Error> {
        Err(Error::IllegalOperationTarget)
    }

    /// Writes the bytes in `buffer` and returns the number of bytes written.
    async fn write(&self, credentials: Credentials, buffer: &[u8]) -> Result<usize, Error> {
        Err(Error::IllegalOperationTarget)
    }
}

/// Flattens the doubly-nested result of a single-action `submit_async`.
fn flatten<T>(result: hel::Result<hel::Result<T>>) -> hel::Result<T> {
    result.and_then(|inner| inner)
}

async fn send_response(
    conversation: &hel::Handle,
    resp: &bindings::SvrResponse,
) -> Result<(), ServeError> {
    let head = bragi::head_to_bytes(resp)?;
    flatten(hel::submit_async(conversation, hel::SendBuffer::new(&head)).await)?;
    Ok(())
}

async fn extract_credentials(conversation: &hel::Handle) -> Result<Credentials, ServeError> {
    Ok(flatten(
        hel::submit_async(conversation, hel::ExtractCredentials).await,
    )?)
}

async fn handle_seek(
    conversation: hel::Handle,
    file: Rc<dyn File>,
    req: bindings::CntRequest,
) -> Result<(), ServeError> {
    let offset = req.rel_offset().unwrap_or(0);
    let result = match req.req_type() {
        bindings::CntReqType::SeekAbs => file.seek_abs(offset).await,
        bindings::CntReqType::SeekRel => file.seek_rel(offset).await,
        bindings::CntReqType::SeekEof => file.seek_eof(offset).await,
        _ => unreachable!(),
    };

    let resp = match result {
        Ok(offset) => {
            let mut resp = bindings::SvrResponse::new(Error::Success);
            resp.set_offset(offset as u64);
            resp
        }
        Err(e) => bindings::SvrResponse::new(e),
    };
    send_response(&conversation, &resp).await
}

async fn handle_cnt_request(
    conversation: hel::Handle,
    file: Rc<dyn File>,
    req: bindings::CntRequest,
) -> Result<(), ServeError> {
    match req.req_type() {
        bindings::CntReqType::SeekAbs
        | bindings::CntReqType::SeekRel
        | bindings::CntReqType::SeekEof => handle_seek(conversation, file, req).await,
        req_type => {
            eprintln!("managarm/fs: dismissing unexpected request type {req_type:?}");
            dismiss(conversation).await
        }
    }
}

async fn handle_read(
    conversation: hel::Handle,
    file: Rc<dyn File>,
    req: bindings::ReadRequest,
) -> Result<(), ServeError> {
    let credentials = extract_credentials(&conversation).await?;

    let mut buffer = vec![0u8; req.size() as usize];
    let (resp, size) = match file.read(credentials, &mut buffer).await {
        Ok(size) => {
            assert!(size <= buffer.len());
            (bindings::SvrResponse::new(Error::Success), size)
        }
        Err(e) => (bindings::SvrResponse::new(e), 0),
    };

    let head = bragi::head_to_bytes(&resp)?;
    let (send_head, send_data) = hel::submit_async(
        &conversation,
        (
            hel::SendBuffer::new(&head),
            hel::SendBuffer::new(&buffer[..size]),
        ),
    )
    .await?;
    send_head.and(send_data)?;
    Ok(())
}

async fn handle_write(
    conversation: hel::Handle,
    file: Rc<dyn File>,
    req: bindings::WriteRequest,
) -> Result<(), ServeError> {
    let mut buffer = vec![0u8; req.size() as usize];
    let (credentials, received) = hel::submit_async(
        &conversation,
        (
            hel::ExtractCredentials,
            hel::ReceiveBuffer::new(&mut buffer),
        ),
    )
    .await?;
    let credentials = credentials?;
    let received = received?;

    let resp = match file.write(credentials, &buffer[..received]).await {
        Ok(size) => {
            let mut resp = bindings::SvrResponse::new(Error::Success);
            resp.set_size(size as i64);
            resp
        }
        Err(e) => bindings::SvrResponse::new(e),
    };
    send_response(&conversation, &resp).await
}

// TODO: Fire a cancellation event once cancellation support is implemented.
async fn handle_cancel(
    conversation: hel::Handle,
    req: bindings::CancelOperation,
) -> Result<(), ServeError> {
    extract_credentials(&conversation).await?;
    eprintln!(
        "managarm/fs: ignoring CancelOperation for cancellation ID {}",
        req.cancellation_id()
    );
    // CancelOperation expects no response.
    Ok(())
}

async fn dismiss(conversation: hel::Handle) -> Result<(), ServeError> {
    flatten(hel::submit_async(&conversation, hel::Dismiss).await)?;
    Ok(())
}

/// Logs any error produced by a request handler running on a detached task.
async fn log_errors(fut: impl Future<Output = Result<(), ServeError>>) {
    if let Err(e) = fut.await {
        eprintln!("managarm/fs: {e}");
    }
}

fn parse_and_spawn<M, F, Fut>(
    head: &[u8],
    conversation: hel::Handle,
    handler: F,
) -> Result<(), ServeError>
where
    M: Default + Message,
    F: FnOnce(hel::Handle, M) -> Fut,
    Fut: Future<Output = Result<(), ServeError>> + 'static,
{
    let req = bragi::head_from_bytes::<M>(head).map_err(ServeError::Malformed)?;
    hel::spawn(log_errors(handler(conversation, req)));
    Ok(())
}

/// Accepts a single conversation, receives the request head and dispatches it to
/// a detached handler task.
///
/// Returns [`ServeError::Shutdown`] once the lane has been shut down; every other
/// error is a per-request failure that leaves the serve loop able to continue.
async fn dispatch_request(lane: &hel::Handle, file: &Rc<dyn File>) -> Result<(), ServeError> {
    let (accept, (head,)) =
        hel::submit_async(lane, hel::Accept::new((hel::ReceiveInline,))).await?;

    // Only the accept action can signal shutdown; any other failure is an IPC error.
    let conversation = match accept {
        Ok(Some(conversation)) => conversation,
        Ok(None) => return Err(ServeError::Transport(hel::Error::IllegalState)),
        Err(hel::Error::LaneShutdown | hel::Error::EndOfLane) => return Err(ServeError::Shutdown),
        Err(e) => return Err(ServeError::Transport(e)),
    };
    let head = head?;

    let preamble = bragi::preamble_from_bytes(&head).map_err(ServeError::Malformed)?;

    match preamble.id() {
        bindings::CntRequest::MESSAGE_ID => {
            let file = file.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_cnt_request(conversation, file, req)
            })?;
        }
        bindings::ReadRequest::MESSAGE_ID => {
            let file = file.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_read(conversation, file, req)
            })?;
        }
        bindings::WriteRequest::MESSAGE_ID => {
            let file = file.clone();
            parse_and_spawn(&head, conversation, move |conversation, req| {
                handle_write(conversation, file, req)
            })?;
        }
        bindings::CancelOperation::MESSAGE_ID => {
            parse_and_spawn(&head, conversation, handle_cancel)?;
        }
        // A decodable but unhandled message ID: unsupported rather than malformed.
        id => eprintln!("managarm/fs: dropping request with unexpected message ID {id}"),
    }
    Ok(())
}

/// Serves the fs passthrough protocol on the given lane until it is shut down.
///
/// Each request is handled on a detached task so that a blocking operation
/// (e.g. a read that waits for data) does not stall subsequent requests.
pub async fn serve_passthrough(lane: hel::Handle, file: Rc<dyn File>) {
    loop {
        match dispatch_request(&lane, &file).await {
            Ok(()) => {}
            Err(ServeError::Shutdown) => return,
            Err(e) => eprintln!("managarm/fs: {e}"),
        }
    }
}
