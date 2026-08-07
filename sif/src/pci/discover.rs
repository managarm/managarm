use std::sync::Mutex;

use super::{EXPECT_LOCK, PciBus};

static ALL_ROOT_BUSES: Mutex<Vec<&'static PciBus>> = Mutex::new(Vec::new());

pub fn all_root_buses() -> Vec<&'static PciBus> {
    ALL_ROOT_BUSES.lock().expect(EXPECT_LOCK).clone()
}

pub fn add_root_bus(bus: &'static PciBus) {
    ALL_ROOT_BUSES.lock().expect(EXPECT_LOCK).push(bus);
}
