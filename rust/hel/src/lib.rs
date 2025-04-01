//! A Rust wrapper for Hel.
#![feature(local_waker)]

pub mod executor;
pub mod handle;
pub mod mapping;
pub mod queue;
pub mod result;
pub mod submission;

use std::time::Duration;

pub use executor::*;
pub use handle::*;
pub use mapping::*;
pub use queue::*;
pub use result::*;
pub use submission::*;

/// A time value in nanoseconds since boot.
#[repr(transparent)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub struct Time(u64);

impl Time {
    /// Returns the current value of the system-wide clock in nanoseconds since boot.
    pub fn now() -> Result<Self> {
        let mut clock = 0;

        hel_check(unsafe { hel_sys::helGetClock(&mut clock) }).map(|_| Self(clock))
    }

    /// Returns the value of the clock in nanoseconds since boot.
    pub fn value(&self) -> u64 {
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
