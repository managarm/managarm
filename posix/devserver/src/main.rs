//! posix-devserver: device manager for POSIX devices.
//!
//! posix-devserver is responsible for:
//! - Enumerating devices from mbus
//! - Instructing posix-subsystem to create/destory device nodes
//! - Serving sysfs
//! - Emitting uevents

mod device;
mod subsystem;
mod sysfs;

use std::sync::Arc;

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

pub async fn log_subsystem_errors(name: &'static str, task: impl Future<Output = Result<()>>) {
    if let Err(e) = task.await {
        eprintln!("devserver: {name} subsystem failed: {e:#}");
    }
}

/// Starts the per-subsystem mbus enumeration loops.
fn start_discovery(model: Arc<Model>) {
    macro_rules! spawn_subsystem {
        ($name:literal, $run:path) => {
            hel::spawn(log_subsystem_errors($name, $run(model.clone())));
        };
    }
    spawn_subsystem!("pci", subsystem::pci::run);
    spawn_subsystem!("acpi", subsystem::acpi::run);
    spawn_subsystem!("usb", subsystem::usb::run);
    spawn_subsystem!("block", subsystem::block::run);
    spawn_subsystem!("nvme", subsystem::nvme::run);
    spawn_subsystem!("drm", subsystem::drm::run);
    spawn_subsystem!("graphics", subsystem::graphics::run);
    spawn_subsystem!("input", subsystem::input::run);
    spawn_subsystem!("net", subsystem::net::run);
    spawn_subsystem!("usbmisc", subsystem::usbmisc::run);
    spawn_subsystem!("sound", subsystem::sound::run);
    spawn_subsystem!("power_supply", subsystem::power_supply::run);
    spawn_subsystem!("tty", subsystem::tty::run);
    spawn_subsystem!("generic", subsystem::generic::run);
    spawn_subsystem!("dmi", subsystem::dmi::run);
    #[cfg(any(target_arch = "aarch64", target_arch = "riscv64"))]
    spawn_subsystem!("dt", subsystem::dt::run);
}

async fn run() -> Result<()> {
    start_discovery(Model::new());
    std::future::pending().await
}

fn main() -> Result<()> {
    println!("devserver: Starting up");
    hel::block_on(run())?
}
