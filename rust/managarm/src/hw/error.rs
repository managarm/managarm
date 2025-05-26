use super::bindings::Errors;

#[derive(Debug)]
pub enum Error {
    OutOfBounds,
    IllegalArguments,
    ResourceExhaustion,
    DeviceError,
    HelError(hel::Error),
    IoError(std::io::Error),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::OutOfBounds => write!(f, "Out of bounds"),
            Error::IllegalArguments => write!(f, "Illegal arguments"),
            Error::ResourceExhaustion => write!(f, "Resource exhaustion"),
            Error::DeviceError => write!(f, "Device error"),
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
            Errors::OutOfBounds => Error::OutOfBounds,
            Errors::IllegalArguments => Error::IllegalArguments,
            Errors::ResourceExhaustion => Error::ResourceExhaustion,
            Errors::DeviceError => Error::DeviceError,
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
