//! Safe wrappers around uACPI.
//!
//! Wrappers are named after the uacpi_*() functions that they wrap. To keep this module
//! extractable into a crate of its own, it must not depend on the rest of sif.

pub mod table;

use std::borrow::Cow;
use std::ffi::CStr;

use thiserror::Error;

use uacpi_sys::uacpi_status;

/// A failure that a uACPI function reported through its status code.
#[derive(Debug, Error)]
#[error("{function}() failed: {}", status_to_string(*.status))]
pub struct Error {
    function: &'static str,
    status: uacpi_status,
}

impl Error {
    fn new(function: &'static str, status: uacpi_status) -> Error {
        Error { function, status }
    }
}

pub type Result<T> = std::result::Result<T, Error>;

/// Wraps uacpi_status_to_string().
fn status_to_string(status: uacpi_status) -> Cow<'static, str> {
    // SAFETY: uACPI returns a string constant for every status code.
    let string: &'static CStr =
        unsafe { CStr::from_ptr(uacpi_sys::uacpi_status_to_string(status)) };
    string.to_string_lossy()
}
