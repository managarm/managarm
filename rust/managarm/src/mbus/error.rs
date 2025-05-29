use super::bindings;

#[derive(Debug)]
pub enum Error {
    NoSuchEntity,
    HelError(hel::Error),
    IoError(std::io::Error),
}

impl std::fmt::Display for Error {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Error::NoSuchEntity => write!(f, "No such entity"),
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

impl From<bindings::Error> for Error {
    fn from(value: bindings::Error) -> Self {
        match value {
            bindings::Error::Success => unreachable!(),
            bindings::Error::NoSuchEntity => Error::NoSuchEntity,
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
