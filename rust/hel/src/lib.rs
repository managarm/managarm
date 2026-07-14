//! A Rust wrapper for Hel.
#![allow(incomplete_features)]
#![feature(maybe_uninit_slice)]
#![feature(generic_const_exprs)]
#![feature(local_waker)]

pub mod executor;
pub mod handle;
pub mod mapping;
pub mod queue;
pub mod result;
pub mod submission;

use std::time::Duration;

pub use executor::{block_on, spawn};
pub use handle::Handle;
pub use mapping::{Mapping, MappingFlags};
pub use queue::Queue;
pub use result::{Error, Result};
pub use submission::{
    action::{
        Accept, Offer, PullDescriptor, PushDescriptor, ReceiveBuffer, ReceiveInline, SendBuffer,
    },
    sleep_for, sleep_until, submit_async,
};

/// Creates a pair of connected lanes that can be used to communicate.
pub fn create_stream() -> Result<(Handle, Handle)> {
    let mut lane1 = hel_sys::kHelNullHandle as hel_sys::HelHandle;
    let mut lane2 = hel_sys::kHelNullHandle as hel_sys::HelHandle;

    result::hel_check(unsafe { hel_sys::helCreateStream(&mut lane1, &mut lane2, 0) })?;

    // SAFETY: helCreateStream returns two freshly created handles in the current universe.
    Ok(unsafe { (Handle::from_raw(lane1), Handle::from_raw(lane2)) })
}

/// A time value in nanoseconds since boot.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Time(u64);

impl Time {
    /// Creates a new [`Time`] instance representing the current time
    /// since boot in nanoseconds.
    pub fn new_since_boot() -> Result<Self> {
        let mut nanos = 0;

        result::hel_check(unsafe { hel_sys::helGetClock(&mut nanos) }).map(|_| Self(nanos))
    }

    /// Creates a new [`Time`] instance from the given number of
    /// nanoseconds since boot.
    pub fn from_nanos(nanos: u64) -> Self {
        Self(nanos)
    }

    /// Returns the value of the clock in nanoseconds since boot.
    pub fn nanos(&self) -> u64 {
        self.0
    }
}

impl std::ops::Add<Duration> for Time {
    type Output = Self;

    fn add(self, rhs: Duration) -> Self::Output {
        Self(self.0 + rhs.as_nanos() as u64)
    }
}

impl std::ops::Sub<Duration> for Time {
    type Output = Self;

    fn sub(self, rhs: Duration) -> Self::Output {
        Self(self.0 - rhs.as_nanos() as u64)
    }
}
