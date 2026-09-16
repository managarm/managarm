//! posix-devserver: device manager for POSIX devices.
//!
//! posix-devserver is responsible for:
//! - Enumerating devices from mbus
//! - Instructing posix-subsystem to create/destory device nodes
//! - Serving sysfs
//! - Emitting uevents

mod device;
mod sysfs;

use anyhow::Result;

use device::{Device, Model};
use sysfs::SysfsNode;

pub(crate) const EXPECT_LOCK: &str = "a devserver mutex was poisoned";

// Statically assert that our device model objects are Send + Sync.
const _: () = {
    fn check<T: Send + Sync>() {}
    let _ = check::<Model>;
    let _ = check::<Device>;
    let _ = check::<SysfsNode>;
};

fn main() -> Result<()> {
    println!("devserver: Starting up");
    let _model = Model::new();
    Ok(())
}
