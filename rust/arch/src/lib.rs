//! A Rust port of libarch.
//!
//! The types here mirror the `arch::` namespace of the C++ library: registers and the fields that
//! they consist of, and the memory and I/O spaces that address them.

#![no_std]

pub(crate) mod sealed {
    pub trait Sealed {}
}

mod bits;
mod register;
mod scalar;

pub use bits::{BitConvertible, BitValue, Field};
pub use register::Register;
pub use scalar::Scalar;
