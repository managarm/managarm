use super::bindings::Errors;

#[derive(Debug)]
pub enum Error {
    Stall,
    Babble,
    Timeout,
    Unsupported,
    Other,
    IllegalRequest,
    /// The device sent a response that does not follow the protocol.
    Malformed,
    HelError(hel::Error),
    IoError(std::io::Error),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::Stall => write!(f, "Stall"),
            Error::Babble => write!(f, "Babble"),
            Error::Timeout => write!(f, "Timeout"),
            Error::Unsupported => write!(f, "Unsupported"),
            Error::Other => write!(f, "Other error"),
            Error::IllegalRequest => write!(f, "Illegal request"),
            Error::Malformed => write!(f, "Malformed response"),
            Error::HelError(err) => write!(f, "Hel error: {:?}", err),
            Error::IoError(err) => write!(f, "IO error: {}", err),
        }
    }
}

impl std::error::Error for Error {
    fn source(&self) -> Option<&(dyn std::error::Error + 'static)> {
        if let Error::IoError(err) = self {
            Some(err)
        } else {
            None
        }
    }
}

impl From<Errors> for Error {
    fn from(value: Errors) -> Self {
        match value {
            Errors::Success => unreachable!(),
            Errors::Stall => Error::Stall,
            Errors::Babble => Error::Babble,
            Errors::Timeout => Error::Timeout,
            Errors::Unsupported => Error::Unsupported,
            Errors::Other => Error::Other,
            Errors::IllegalRequest => Error::IllegalRequest,
        }
    }
}

impl From<hel::Error> for Error {
    fn from(err: hel::Error) -> Self {
        Error::HelError(err)
    }
}

impl From<std::io::Error> for Error {
    fn from(err: std::io::Error) -> Self {
        Error::IoError(err)
    }
}
