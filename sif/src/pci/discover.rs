use std::sync::Mutex;
use std::sync::atomic::Ordering;

use super::{
    BarType, Capability, EXPECT_LOCK, IrqIndex, PCI_REGULAR_BAR0, PciBridge, PciBus, PciDevice,
    PciEntity, name_of_capability,
};

use crate::acpi::PAGE_MASK;

static ALL_DEVICES: Mutex<Vec<&'static PciDevice>> = Mutex::new(Vec::new());
static ALL_ROOT_BUSES: Mutex<Vec<&'static PciBus>> = Mutex::new(Vec::new());

pub fn all_devices() -> Vec<&'static PciDevice> {
    ALL_DEVICES.lock().expect(EXPECT_LOCK).clone()
}

pub fn all_root_buses() -> Vec<&'static PciBus> {
    ALL_ROOT_BUSES.lock().expect(EXPECT_LOCK).clone()
}

pub fn add_root_bus(bus: &'static PciBus) {
    ALL_ROOT_BUSES.lock().expect(EXPECT_LOCK).push(bus);
}

fn compute_bar_length(mask: u64) -> u64 {
    assert!(mask != 0);
    1u64 << mask.trailing_zeros()
}

fn read_entity_bars(entity: &PciEntity, n_bars: usize) {
    let bus = entity.parent_bus;
    let slot = entity.slot;
    let function = entity.function;

    let mut bars = entity.bars.lock().expect(EXPECT_LOCK);

    // The caller passes the number of BARs of the header type of the entity, hence all
    // offsets that we compute below address BARs.
    let size_bar = |offset: u16, restore: u32| -> u32 {
        unsafe {
            bus.set_bar(slot, function, offset, 0xFFFFFFFF);
            let mask = bus.bar(slot, function, offset);
            bus.set_bar(slot, function, offset, restore);

            mask
        }
    };

    let mut i = 0;
    while i < n_bars {
        let offset = PCI_REGULAR_BAR0 + (i as u16) * 4;
        let bar = unsafe { bus.bar(slot, function, offset) };

        if bar & 1 != 0 {
            let address = (bar & 0xFFFFFFFC) as u64;
            let mask = size_bar(offset, bar) & 0xFFFFFFFC;

            // The device does not decode any address bits from this BAR.
            if mask == 0 {
                i += 1;
                continue;
            }

            let length = compute_bar_length(mask as u64);

            bars[i].type_ = BarType::Io;
            bars[i].address = address;
            bars[i].length = length;

            if address == 0 {
                println!("sif:     unallocated I/O space BAR #{i}, length: {length} ports");
            } else {
                bars[i].host_type = BarType::Io;
                bars[i].host_address = address;
                bars[i].offset = 0;

                println!("sif:     I/O space BAR #{i} at {address:#x}, length: {length} ports");
            }

            i += 1;
        } else if (bar >> 1) & 3 == 0 {
            let address = (bar & 0xFFFFFFF0) as u64;
            let mask = size_bar(offset, bar) & 0xFFFFFFF0;

            // The device does not decode any address bits from this BAR.
            if mask == 0 {
                i += 1;
                continue;
            }

            let length = compute_bar_length(mask as u64);
            let prefetchable = bar & (1 << 3) != 0;

            bars[i].type_ = BarType::Memory;
            bars[i].address = address;
            bars[i].length = length;
            bars[i].prefetchable = prefetchable;

            if address == 0 {
                println!(
                    "sif:     unallocated 32-bit memory BAR #{i}, length: {length} bytes{}",
                    if prefetchable { " (prefetchable)" } else { "" }
                );
            } else {
                bars[i].host_type = BarType::Memory;
                bars[i].host_address = address;
                bars[i].offset = (address as usize & PAGE_MASK) as u32;

                println!(
                    "sif:     32-bit memory BAR #{i} at {address:#x}, length: {length} bytes{}",
                    if prefetchable { " (prefetchable)" } else { "" }
                );
            }

            i += 1;
        } else if (bar >> 1) & 3 == 2 {
            assert!(i < n_bars - 1); // Otherwise there is no next BAR.
            let high = unsafe { bus.bar(slot, function, offset + 4) };
            let address = ((high as u64) << 32) | (bar & 0xFFFFFFF0) as u64;

            let mask = unsafe {
                bus.set_bar(slot, function, offset, 0xFFFFFFFF);
                bus.set_bar(slot, function, offset + 4, 0xFFFFFFFF);
                let mask = ((bus.bar(slot, function, offset + 4) as u64) << 32)
                    | (bus.bar(slot, function, offset) & 0xFFFFFFF0) as u64;
                bus.set_bar(slot, function, offset, bar);
                bus.set_bar(slot, function, offset + 4, high);

                mask
            };

            // The device does not decode any address bits from this BAR.
            if mask == 0 {
                i += 2;
                continue;
            }

            let length = compute_bar_length(mask);
            let prefetchable = bar & (1 << 3) != 0;

            bars[i].type_ = BarType::Memory;
            bars[i].address = address;
            bars[i].length = length;
            bars[i].prefetchable = prefetchable;

            if address == 0 {
                println!(
                    "sif:     unallocated 64-bit memory BAR #{i}, length: {length} bytes{}",
                    if prefetchable { " (prefetchable)" } else { "" }
                );
            } else {
                bars[i].host_type = BarType::Memory;
                bars[i].host_address = address;
                bars[i].offset = (address as usize & PAGE_MASK) as u32;

                println!(
                    "sif:     64-bit memory BAR #{i} at {address:#x}, length: {length} bytes{}",
                    if prefetchable { " (prefetchable)" } else { "" }
                );
            }

            i += 2;
        } else {
            panic!("Unexpected BAR type");
        }
    }
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

        read_entity_bars(&device.entity, 6);

        let irq_index = IrqIndex::from_pin(bus.interrupt_pin(slot, function));
        if irq_index != IrqIndex::Null {
            if let Some(gsi) = super::acpi::resolve_irq(bus.seg_id, slot, irq_index) {
                println!(
                    "sif:     Interrupt: {} (routed to GSI {gsi})",
                    irq_index.name()
                );
                device
                    .interrupt
                    .set(gsi)
                    .expect("sif: PCI device was already enumerated");
            } else {
                println!("sif:     Interrupt routing not available!");
            }
        }

        ALL_DEVICES.lock().expect(EXPECT_LOCK).push(device);
        bus.child_devices.lock().expect(EXPECT_LOCK).push(device);
    } else if header_type & 0x7F == 1 {
        let bridge = PciBridge::new(
            bus, slot, function, vendor, device_id, revision, class_code, sub_class, interface,
        );
        bus.child_bridges.lock().expect(EXPECT_LOCK).push(bridge);

        find_pci_caps(&bridge.entity);

        read_entity_bars(&bridge.entity, 2);

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
