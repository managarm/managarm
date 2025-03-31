//! A Rust wrapper for Hel.

pub mod executor;
pub mod handle;
pub mod mapping;
pub mod queue;
pub mod result;
pub mod submission;

pub use executor::*;
pub use handle::*;
pub use mapping::*;
pub use queue::*;
pub use result::*;
pub use submission::*;

/// Aligns a value up to the nearest multiple of the specified alignment.
/// The alignment must be a power of two.
fn align_up(value: usize, alignment: usize) -> usize {
    (value + alignment - 1) & !(alignment - 1)
}

/// Returns the current value of the system-wide clock in nanoseconds since boot.
pub fn get_clock() -> Result<u64> {
    let mut clock = 0;

    hel_check(unsafe { hel_sys::helGetClock(&mut clock) }).map(|_| clock)
}
