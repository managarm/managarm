//! Error handling for the Hel API.
//! Contains the `Error` enum and the `Result` type alias.

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Error {
    IllegalSyscall,
    IllegalArgs,
    IllegalState,
    UnsupportedOperation,
    OutOfBounds,
    QueueTooSmall,
    Cancelled,
    NoDescriptor,
    BadDescriptor,
    ThreadTerminated,
    TransmissionMismatch,
    LaneShutdown,
    EndOfLane,
    Dismissed,
    BufferTooSmall,
    Fault,
    RemoteFault,
    NoHardwareSupport,
    NoMemory,
    AlreadyExists,
}

impl From<hel_sys::HelError> for Error {
    fn from(error: hel_sys::HelError) -> Self {
        match error as u32 {
            hel_sys::kHelErrIllegalSyscall => Error::IllegalSyscall,
            hel_sys::kHelErrIllegalArgs => Error::IllegalArgs,
            hel_sys::kHelErrIllegalState => Error::IllegalState,
            hel_sys::kHelErrUnsupportedOperation => Error::UnsupportedOperation,
            hel_sys::kHelErrOutOfBounds => Error::OutOfBounds,
            hel_sys::kHelErrQueueTooSmall => Error::QueueTooSmall,
            hel_sys::kHelErrCancelled => Error::Cancelled,
            hel_sys::kHelErrNoDescriptor => Error::NoDescriptor,
            hel_sys::kHelErrBadDescriptor => Error::BadDescriptor,
            hel_sys::kHelErrThreadTerminated => Error::ThreadTerminated,
            hel_sys::kHelErrTransmissionMismatch => Error::TransmissionMismatch,
            hel_sys::kHelErrLaneShutdown => Error::LaneShutdown,
            hel_sys::kHelErrEndOfLane => Error::EndOfLane,
            hel_sys::kHelErrDismissed => Error::Dismissed,
            hel_sys::kHelErrBufferTooSmall => Error::BufferTooSmall,
            hel_sys::kHelErrFault => Error::Fault,
            hel_sys::kHelErrRemoteFault => Error::RemoteFault,
            hel_sys::kHelErrNoHardwareSupport => Error::NoHardwareSupport,
            hel_sys::kHelErrNoMemory => Error::NoMemory,
            hel_sys::kHelErrAlreadyExists => Error::AlreadyExists,
            _ => unreachable!(),
        }
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// Utility function to check Hel errors and convert them to `Result`.
pub(crate) fn hel_check(error: hel_sys::HelError) -> Result<()> {
    if error as u32 == hel_sys::kHelErrNone {
        Ok(())
    } else {
        Err(Error::from(error))
    }
}
