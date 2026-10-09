use super::bindings::Errors;

#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("Out of bounds")]
    OutOfBounds,
    #[error("Illegal arguments")]
    IllegalArguments,
    #[error("Resource exhaustion")]
    ResourceExhaustion,
    #[error("Device error")]
    DeviceError,
    #[error("Property not found")]
    PropertyNotFound,
    #[error(transparent)]
    HelError(#[from] hel::Error),
    #[error(transparent)]
    IoError(#[from] std::io::Error),
}

impl From<Errors> for Error {
    fn from(value: Errors) -> Self {
        match value {
            Errors::Success => unreachable!(),
            Errors::OutOfBounds => Error::OutOfBounds,
            Errors::IllegalArguments => Error::IllegalArguments,
            Errors::ResourceExhaustion => Error::ResourceExhaustion,
            Errors::DeviceError => Error::DeviceError,
            Errors::PropertyNotFound => Error::PropertyNotFound,
        }
    }
}
