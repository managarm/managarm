use std::sync::Mutex;
use std::sync::atomic::Ordering;

use super::{Capability, EXPECT_LOCK, PciBridge, PciBus, PciDevice, PciEntity, name_of_capability};

static ALL_DEVICES: Mutex<Vec<&'static PciDevice>> = Mutex::new(Vec::new());
static ALL_ROOT_BUSES: Mutex<Vec<&'static PciBus>> = Mutex::new(Vec::new());

pub fn all_root_buses() -> Vec<&'static PciBus> {
    ALL_ROOT_BUSES.lock().expect(EXPECT_LOCK).clone()
}

pub fn add_root_bus(bus: &'static PciBus) {
    ALL_ROOT_BUSES.lock().expect(EXPECT_LOCK).push(bus);
}

fn find_pci_caps(entity: &PciEntity) {
    let bus = entity.parent_bus;
    let slot = entity.slot;
    let function = entity.function;

    let status = bus.status(slot, function);

    // Find all capabilities.
    if status & 0x10 != 0 {
        // The bottom two bits of each capability offset must be masked!
        let mut offset = (bus.capabilities_pointer(slot, function) & 0xFC) as u16;
        while offset != 0 {
            // Capability headers sit at device-defined offsets, hence we cannot use a
            // safe accessor to read them.
            let ent = unsafe { bus.read_config_half(slot, function, offset) };
            let type_ = (ent & 0xFF) as u32;

            if let Some(name) = name_of_capability(type_) {
                println!("sif:     {name} capability");
            } else {
                println!("sif:     Capability of type {type_:#04x}");
            }

            let length = if type_ == 0x09 {
                Some(unsafe { bus.read_config_byte(slot, function, offset + 2) } as u64)
            } else {
                None
            };

            entity.caps.lock().expect(EXPECT_LOCK).push(Capability {
                type_,
                offset,
                length,
            });

            offset = ((ent >> 8) & 0xFC) as u16;
        }
    }
}

fn check_pci_function(
    bus: &'static PciBus,
    slot: u8,
    function: u8,
    enumerate_downstream: &mut dyn FnMut(&'static PciBus),
) {
    let vendor = bus.vendor(slot, function);
    if vendor == 0xFFFF {
        return;
    }

    let header_type = bus.header_type(slot, function);
    if header_type & 0x7F == 0 {
        println!("sif:   Function {function}: Device");
    } else if header_type & 0x7F == 1 {
        let downstream_id = unsafe { bus.secondary_bus(slot, function) };

        if downstream_id == 0 {
            println!("sif:   Function {function}: unconfigured PCI-to-PCI bridge");
        } else {
            println!("sif:   Function {function}: PCI-to-PCI bridge to bus {downstream_id}");
        }
    } else {
        println!(
            "sif:   Function {function}: Unexpected PCI header type {}",
            header_type & 0x7F
        );
    }

    let device_id = bus.device_id(slot, function);
    let revision = bus.revision(slot, function);
    let class_code = bus.class_code(slot, function);
    let sub_class = bus.sub_class(slot, function);
    let interface = bus.interface(slot, function);

    println!(
        "sif:     Vendor/device: {vendor:04x}.{device_id:04x}.{revision:02x}, \
                class: {class_code:02x}.{sub_class:02x}.{interface:02x}"
    );

    if header_type & 0x7F == 0 {
        let subsystem_vendor = unsafe { bus.subsystem_vendor(slot, function) };
        let subsystem_device = unsafe { bus.subsystem_device(slot, function) };

        let status = bus.status(slot, function);
        if status & 0x08 != 0 {
            println!("sif:       IRQ is asserted!");
        }

        let device = PciDevice::new(
            bus,
            slot,
            function,
            vendor,
            device_id,
            revision,
            class_code,
            sub_class,
            interface,
            subsystem_vendor,
            subsystem_device,
        );

        find_pci_caps(&device.entity);

        ALL_DEVICES.lock().expect(EXPECT_LOCK).push(device);
        bus.child_devices.lock().expect(EXPECT_LOCK).push(device);
    } else if header_type & 0x7F == 1 {
        let bridge = PciBridge::new(
            bus, slot, function, vendor, device_id, revision, class_code, sub_class, interface,
        );
        bus.child_bridges.lock().expect(EXPECT_LOCK).push(bridge);

        find_pci_caps(&bridge.entity);

        let downstream_id = unsafe { bus.secondary_bus(slot, function) };

        if downstream_id != 0 {
            bridge.downstream_id.store(downstream_id, Ordering::Relaxed);
            bridge.subordinate_id.store(
                unsafe { bus.subordinate_bus(slot, function) },
                Ordering::Relaxed,
            );

            let downstream_bus = bus.make_downstream_bus(bridge, downstream_id);
            assert!(
                bridge.associated_bus.set(downstream_bus).is_ok(),
                "sif: PCI bridge was already enumerated"
            );
            enumerate_downstream(downstream_bus);
        } else {
            println!("sif:     Deferring enumeration until the bridge is configured");
        }
    }
}

fn check_pci_device(
    bus: &'static PciBus,
    slot: u8,
    enumerate_downstream: &mut dyn FnMut(&'static PciBus),
) {
    let vendor = bus.vendor(slot, 0);
    if vendor == 0xFFFF {
        return;
    }

    println!(
        "sif: Segment: {}, bus: {}, slot {}",
        bus.seg_id, bus.bus_id, slot
    );

    let header_type = bus.header_type(slot, 0);
    if header_type & 0x80 != 0 {
        for function in 0..8 {
            check_pci_function(bus, slot, function, enumerate_downstream);
        }
    } else {
        check_pci_function(bus, slot, 0, enumerate_downstream);
    }
}

fn check_pci_bus(bus: &'static PciBus, enumerate_downstream: &mut dyn FnMut(&'static PciBus)) {
    for slot in 0..32 {
        check_pci_device(bus, slot, enumerate_downstream);
    }
}

pub fn enumerate_all() {
    // Downstream buses are appended as we go, hence this also covers buses behind bridges.
    let mut queue = all_root_buses();
    let mut i = 0;
    while i < queue.len() {
        let bus = queue[i];
        check_pci_bus(bus, &mut |downstream| queue.push(downstream));
        i += 1;
    }
}
