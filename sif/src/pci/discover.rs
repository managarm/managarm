use std::sync::Mutex;
use std::sync::atomic::Ordering;

use super::{
    BarType, Capability, EXPECT_LOCK, ExtendedCapability, IrqIndex, PCI_REGULAR_BAR0, PciBridge,
    PciBus, PciBusResource, PciDevice, PciEntity, name_of_capability, name_of_extended_capability,
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
    //
    // Sizing a BAR temporarily invalidates its address; disable I/O and memory
    // decode around the probe so the device does not respond to stray accesses.
    let size_bar = |offset: u16, restore: u32| -> u32 {
        let command = bus.command(slot, function);
        if command & 0x03 != 0 {
            bus.set_command(slot, function, command & !0x03);
        }

        let mask = unsafe {
            bus.set_bar(slot, function, offset, 0xFFFFFFFF);
            let mask = bus.bar(slot, function, offset);
            bus.set_bar(slot, function, offset, restore);

            mask
        };

        if command & 0x03 != 0 {
            bus.set_command(slot, function, command);
        }

        mask
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
                // Check the parent resources to see if this BAR is actually memory mapped.
                let mut host = None;
                for res in bus.resources.lock().expect(EXPECT_LOCK).iter() {
                    if res.flags() == PciBusResource::IO
                        && address >= res.base()
                        && address + length <= res.base() + res.size()
                    {
                        host = Some((res.host_base() + (address - res.base()), res.is_host_mmio()));
                        break;
                    }
                }

                if let Some((host_address, true)) = host {
                    bars[i].host_type = BarType::Memory;
                    bars[i].allocated = true;
                    bars[i].host_address = host_address;
                    bars[i].offset = (host_address as usize & PAGE_MASK) as u32;
                } else {
                    bars[i].host_type = BarType::Io;
                    bars[i].allocated = true;
                    bars[i].host_address = address;
                    bars[i].offset = 0;
                }

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
                bars[i].allocated = true;
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

            let command = bus.command(slot, function);
            if command & 0x03 != 0 {
                bus.set_command(slot, function, command & !0x03);
            }

            let mask = unsafe {
                bus.set_bar(slot, function, offset, 0xFFFFFFFF);
                bus.set_bar(slot, function, offset + 4, 0xFFFFFFFF);
                let mask = ((bus.bar(slot, function, offset + 4) as u64) << 32)
                    | (bus.bar(slot, function, offset) & 0xFFFFFFF0) as u64;
                bus.set_bar(slot, function, offset, bar);
                bus.set_bar(slot, function, offset + 4, high);

                mask
            };

            if command & 0x03 != 0 {
                bus.set_command(slot, function, command);
            }

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
                bars[i].allocated = true;
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

            if type_ == 0x10 {
                entity.is_pcie.store(true, Ordering::Relaxed);

                let flags = unsafe { bus.read_config_half(slot, function, offset + 2) };
                let port_type = (flags >> 4) & 0xF;
                entity.is_downstream_port.store(
                    port_type == 4 // Root port
                    || port_type == 6 // Downstream port
                    || port_type == 8, // PCI(-X) to PCIe bridge
                    Ordering::Relaxed,
                );
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

    // PCIe devices are required to provide the 4096-byte configuration space.
    if bus.io.supports_4k_config_space() && entity.is_pcie.load(Ordering::Relaxed) {
        let mut offset: u16 = 0x100;

        while offset != 0 {
            // Extended capability headers sit at device-defined offsets, hence we cannot
            // use a safe accessor to read them.
            let data = unsafe { bus.read_config_word(slot, function, offset) };
            let extended_cap_id = (data & 0xFFFF) as u16;
            let version = (data >> 16) & 0xF;
            // The bottom 2 bits are reserved and must be masked out.
            // This offset is relative to the start of the entire configuration space.
            let next_offset = ((data >> 20) & 0xFFC) as u16;
            if next_offset > 0 && next_offset < 0x100 {
                println!("sif: invalid 'Next Capability Offset' {next_offset:#x}, skipping");
                break;
            }

            // Any one of these conditions signals that there are no further extended capabilities.
            if extended_cap_id == 0xFFFF || extended_cap_id == 0 || next_offset == 0 {
                break;
            }

            // We have a valid extended capability.
            if let Some(name) = name_of_extended_capability(extended_cap_id) {
                println!("sif:     {name} Extended Capability (v{version})");
            } else {
                println!("sif:     Extended Capability {extended_cap_id:#x} (v{version})");
            }

            entity
                .extended_caps
                .lock()
                .expect(EXPECT_LOCK)
                .push(ExtendedCapability {
                    type_: extended_cap_id,
                    offset,
                });

            offset = next_offset;
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

    // Disable interrupts and bus mastering until a driver configures the device.
    //
    // We don't disable I/O and memory decoding so any devices in use by the kernel
    // remain functional (e.g. framebuffers or UARTs).
    let mut command = bus.command(slot, function);
    command &= !0x4; // Disable bus mastering.
    command |= 0x400; // Mask IRQs.
    bus.set_command(slot, function, command);

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
            let router = bus.irq_router.get().expect("bus has no IRQ router");
            if let Some(pin) = router.resolve_irq_route(slot, irq_index) {
                println!(
                    "sif:     Interrupt: {} (routed to {})",
                    irq_index.name(),
                    pin.name()
                );
                assert!(
                    device.interrupt.set(pin).is_ok(),
                    "sif: PCI device was already enumerated"
                );
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
    let bridge = bus.associated_bridge;
    let mut n_slots: u8 = 32;

    // A PCIe downstream port has only one device (slot 0) attached.
    // In theory, this is only an optimization, in practice however omitting
    // this causes a SError on the BCM2711 when trying to access the vendor ID
    // of a non-existant device.
    if let Some(bridge) = bridge
        && bridge.entity.is_pcie.load(Ordering::Relaxed)
        && bridge.entity.is_downstream_port.load(Ordering::Relaxed)
    {
        n_slots = 1;
    }

    for slot in 0..n_slots {
        check_pci_device(bus, slot, enumerate_downstream);
    }
}

fn check_for_bridge_resources(bridge: &'static PciBridge) {
    let parent_bus = bridge.entity.parent_bus;
    let slot = bridge.entity.slot;
    let function = bridge.entity.function;
    let downstream = bridge
        .associated_bus
        .get()
        .expect("bridge has no associated bus");

    {
        // The low nibble of the I/O base/limit encodes capabilities, not address bits.
        let base = (unsafe { parent_bus.io_base(slot, function) } & 0xF0) as u64;
        let limit = (unsafe { parent_bus.io_limit(slot, function) } & 0xF0) as u64;

        let addr = base << 8;
        let size = (limit << 8).wrapping_add(0x100).wrapping_sub(addr);

        // Try to look up the host base address in our parent's resources.
        let mut host_base = 0;
        let mut is_host_mmio = false;
        for res in parent_bus.resources.lock().expect(EXPECT_LOCK).iter() {
            if res.base() <= addr
                && res.base() + res.size() >= addr.wrapping_add(size)
                && res.flags() == PciBusResource::IO
            {
                is_host_mmio = res.is_host_mmio();
                host_base = res.host_base() + (addr - res.base());
                break;
            }
        }

        // If the window is not found in the parent's resources, assume it's
        // garbage/empty and ignore it.
        if host_base != 0 && size != 0 {
            println!(
                "sif: Discovered existing I/O window of bridge \
                        {:04x}:{:02x}:{:02x}.{} address: {addr:#x} size: {size} \
                        (host base: {host_base:#x})",
                bridge.entity.seg, bridge.entity.bus, slot, function
            );

            downstream
                .resources
                .lock()
                .expect(EXPECT_LOCK)
                .push(PciBusResource::new(
                    addr,
                    size,
                    host_base,
                    PciBusResource::IO,
                    is_host_mmio,
                ));
        }
    }

    {
        let base = unsafe { parent_bus.mem_base(slot, function) } as u64;
        let limit = unsafe { parent_bus.mem_limit(slot, function) } as u64;

        let addr = base << 16;
        let size = (limit << 16).wrapping_add(0x100000).wrapping_sub(addr);

        // Try to look up the host base address in our parent's resources.
        let mut host_base = 0;
        for res in parent_bus.resources.lock().expect(EXPECT_LOCK).iter() {
            if res.base() <= addr
                && res.base() + res.size() >= addr.wrapping_add(size)
                && res.flags() == PciBusResource::MEMORY
            {
                host_base = res.host_base() + (addr - res.base());
                break;
            }
        }

        // If the window is not found in the parent's resources, assume it's
        // garbage/empty and ignore it.
        if host_base != 0 && size != 0 {
            println!(
                "sif: Discovered existing memory window of bridge \
                        {:04x}:{:02x}:{:02x}.{} address: {addr:#x} size: {size} \
                        (host base: {host_base:#x})",
                bridge.entity.seg, bridge.entity.bus, slot, function
            );

            downstream
                .resources
                .lock()
                .expect(EXPECT_LOCK)
                .push(PciBusResource::new(
                    addr,
                    size,
                    host_base,
                    PciBusResource::MEMORY,
                    true,
                ));
        }
    }

    {
        // The low nibble of the prefetch base/limit encodes 64-bit support, not address bits.
        let base = (unsafe { parent_bus.prefetch_mem_base(slot, function) } & 0xFFF0) as u64;
        let limit = (unsafe { parent_bus.prefetch_mem_limit(slot, function) } & 0xFFF0) as u64;

        let base_upper = unsafe { parent_bus.prefetch_mem_base_upper(slot, function) } as u64;
        let limit_upper = unsafe { parent_bus.prefetch_mem_limit_upper(slot, function) } as u64;

        let addr = (base << 16) | (base_upper << 32);
        let size = ((limit << 16) | (limit_upper << 32))
            .wrapping_add(0x100000)
            .wrapping_sub(addr);

        // Try to look up the host base address in our parent's resources.
        let mut host_base = 0;
        for res in parent_bus.resources.lock().expect(EXPECT_LOCK).iter() {
            if res.base() <= addr
                && res.base() + res.size() >= addr.wrapping_add(size)
                && res.flags() == PciBusResource::PREF_MEMORY
            {
                host_base = res.host_base() + (addr - res.base());
                break;
            }
        }

        // If the window is not found in the parent's resources, assume it's
        // garbage/empty and ignore it.
        if host_base != 0 && size != 0 {
            println!(
                "sif: Discovered existing prefetch memory window of bridge \
                        {:04x}:{:02x}:{:02x}.{} address: {addr:#x} size: {size} \
                        (host base: {host_base:#x})",
                bridge.entity.seg, bridge.entity.bus, slot, function
            );

            downstream
                .resources
                .lock()
                .expect(EXPECT_LOCK)
                .push(PciBusResource::new(
                    addr,
                    size,
                    host_base,
                    PciBusResource::PREF_MEMORY,
                    true,
                ));
        }
    }
}

fn configure_bridges(bus: &'static PciBus, highest_id: &mut u8) {
    let mut i = 0;
    while let Some(bridge) = {
        let bridges = bus.child_bridges.lock().expect(EXPECT_LOCK);
        bridges.get(i).copied()
    } {
        if bridge.downstream_id.load(Ordering::Relaxed) == 0 {
            let parent = bridge.entity.parent_bus.associated_bridge;

            let mut b = parent;
            while let Some(cur) = b {
                let subordinate_id = cur.subordinate_id.load(Ordering::Relaxed);
                println!(
                    "sif: Bumping bridge {:04x}:{:02x}:{:02x}.{} from subordinate id {} \
                            to subordinate id {}",
                    cur.entity.seg,
                    cur.entity.bus,
                    cur.entity.slot,
                    cur.entity.function,
                    subordinate_id,
                    subordinate_id + 1
                );

                cur.subordinate_id
                    .store(subordinate_id + 1, Ordering::Relaxed);
                unsafe {
                    cur.entity.parent_bus.set_subordinate_bus(
                        cur.entity.slot,
                        cur.entity.function,
                        subordinate_id + 1,
                    )
                };
                b = cur.entity.parent_bus.associated_bridge;
            }

            if let Some(parent) = parent {
                let subordinate_id = parent.subordinate_id.load(Ordering::Relaxed);
                assert!(*highest_id < subordinate_id);
                *highest_id = subordinate_id;

                bridge
                    .downstream_id
                    .store(subordinate_id, Ordering::Relaxed);
                bridge
                    .subordinate_id
                    .store(subordinate_id, Ordering::Relaxed);
            } else {
                // We're directly on the root bus.
                // TODO: this ID may be in use by a bridge on a different root bus.
                *highest_id += 1;

                bridge.downstream_id.store(*highest_id, Ordering::Relaxed);
                bridge.subordinate_id.store(*highest_id, Ordering::Relaxed);
            }

            let downstream_id = bridge.downstream_id.load(Ordering::Relaxed);
            let subordinate_id = bridge.subordinate_id.load(Ordering::Relaxed);
            unsafe {
                bridge.entity.parent_bus.set_secondary_bus(
                    bridge.entity.slot,
                    bridge.entity.function,
                    downstream_id,
                );
                bridge.entity.parent_bus.set_subordinate_bus(
                    bridge.entity.slot,
                    bridge.entity.function,
                    subordinate_id,
                );
            }

            println!(
                "sif: Found unconfigured bridge {:04x}:{:02x}:{:02x}.{}, now configured to \
                        downstream {}, subordinate {}",
                bridge.entity.seg,
                bridge.entity.bus,
                bridge.entity.slot,
                bridge.entity.function,
                downstream_id,
                subordinate_id
            );

            let downstream_bus = bus.make_downstream_bus(bridge, downstream_id);
            assert!(
                bridge.associated_bus.set(downstream_bus).is_ok(),
                "sif: PCI bridge was already enumerated"
            );
            check_pci_bus(downstream_bus, &mut |b: &'static PciBus| {
                let br = b.associated_bridge.unwrap();
                panic!(
                    "sif: error: found already configured bridge {:04x}:{:02x}:{:02x}.{} \
                            under an unconfigured bridge",
                    br.entity.seg, br.entity.bus, br.entity.slot, br.entity.function
                );
            });
        }

        let downstream_bus = bridge
            .associated_bus
            .get()
            .expect("Bridge has no associated bus");

        // Look for any existing bridge resources.
        check_for_bridge_resources(bridge);

        configure_bridges(downstream_bus, highest_id);

        i += 1;
    }
}

fn find_highest_id(bus: &'static PciBus) -> u8 {
    let mut id = bus.bus_id;

    for bridge in bus.child_bridges.lock().expect(EXPECT_LOCK).iter() {
        let subordinate_id = bridge.subordinate_id.load(Ordering::Relaxed);
        if subordinate_id == 0 {
            continue;
        }

        if id < subordinate_id {
            id = subordinate_id;
        }
    }

    id
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

    // Configure unconfigured bridges.
    println!("sif: Looking for unconfigured PCI bridges");

    for root_bus in all_root_buses() {
        let mut highest_id = find_highest_id(root_bus);
        configure_bridges(root_bus, &mut highest_id);
    }
}
