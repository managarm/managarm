use std::collections::HashMap;
use std::ffi::c_void;
use std::future::Future;
use std::pin::Pin;
use std::rc::Rc;

use anyhow::Result;
use hel::{IrqPolarity, IrqTrigger};
use managarm::hw::pci::IoType;
use managarm::hw::server::{BarDescriptor, CapDescriptor, PciDevice, serve_pci_device};
use managarm::mbus::{EntityManager, Item, Properties, create_entity};

use uacpi_sys::{uacpi_iteration_decision, uacpi_namespace_node, uacpi_u32, uacpi_u64};

use crate::acpi::{config_read, config_write};

#[derive(Clone, Copy)]
struct Address {
    seg: u16,
    bus: u8,
    slot: u8,
    func: u8,
}

impl Address {
    fn read(self, offset: u16, size: u8) -> u32 {
        config_read(self.seg, self.bus, self.slot, self.func, offset, size)
    }

    fn read8(self, offset: u16) -> u8 {
        self.read(offset, 1) as u8
    }

    fn read16(self, offset: u16) -> u16 {
        self.read(offset, 2) as u16
    }

    fn read32(self, offset: u16) -> u32 {
        self.read(offset, 4)
    }

    fn write32(self, offset: u16, value: u32) {
        config_write(self.seg, self.bus, self.slot, self.func, offset, 4, value);
    }
}

fn size_from_mask(mask: u32) -> u64 {
    if mask == 0 {
        0
    } else {
        1u64 << mask.trailing_zeros()
    }
}

fn size_from_mask64(mask: u64) -> u64 {
    if mask == 0 {
        0
    } else {
        1u64 << mask.trailing_zeros()
    }
}

fn read_bars(addr: Address, count: usize) -> [BarDescriptor; 6] {
    let mut bars = [BarDescriptor::default(); 6];
    let mut i = 0;
    while i < count {
        let offset = 0x10 + (i as u16) * 4;
        let original = addr.read32(offset);
        addr.write32(offset, 0xFFFFFFFF);
        let mask = addr.read32(offset);
        addr.write32(offset, original);

        if mask == 0 {
            i += 1;
            continue;
        }

        if original & 1 != 0 {
            let base = (original & 0xFFFFFFFC) as u64;
            bars[i] = BarDescriptor {
                io_type: IoType::Port,
                host_type: IoType::Port,
                address: base,
                length: size_from_mask(mask & 0xFFFFFFFC),
                offset: 0,
            };
            i += 1;
        } else if (original >> 1) & 3 == 2 {
            let high_offset = offset + 4;
            let high_original = addr.read32(high_offset);
            addr.write32(high_offset, 0xFFFFFFFF);
            let high_mask = addr.read32(high_offset);
            addr.write32(high_offset, high_original);

            let base = (((high_original as u64) << 32) | (original & 0xFFFFFFF0) as u64) as u64;
            let full_mask = ((high_mask as u64) << 32) | (mask & 0xFFFFFFF0) as u64;
            bars[i] = BarDescriptor {
                io_type: IoType::Memory,
                host_type: IoType::Memory,
                address: base,
                length: size_from_mask64(full_mask),
                offset: (base & 0xFFF) as u32,
            };
            i += 2;
        } else {
            let base = (original & 0xFFFFFFF0) as u64;
            bars[i] = BarDescriptor {
                io_type: IoType::Memory,
                host_type: IoType::Memory,
                address: base,
                length: size_from_mask(mask & 0xFFFFFFF0),
                offset: (base & 0xFFF) as u32,
            };
            i += 1;
        }
    }
    bars
}

fn read_capabilities(addr: Address) -> Vec<CapDescriptor> {
    let mut caps = Vec::new();
    if addr.read16(0x06) & 0x10 == 0 {
        return caps;
    }

    let mut pointer = (addr.read8(0x34) & 0xFC) as u16;
    let mut seen = 0;
    while pointer != 0 && seen < 64 {
        let type_ = addr.read8(pointer);
        let next = addr.read8(pointer + 1) & 0xFC;
        let length = if type_ == 0x09 {
            addr.read8(pointer + 2) as u64
        } else {
            0
        };
        caps.push(CapDescriptor {
            type_: type_ as u32,
            offset: pointer as u64,
            length,
        });
        pointer = next as u16;
        seen += 1;
    }
    caps
}

struct SifPciDevice {
    addr: Address,
    irq: Option<u32>,
    bars: [BarDescriptor; 6],
    caps: Vec<CapDescriptor>,
}

impl PciDevice for SifPciDevice {
    fn bars(&self) -> [BarDescriptor; 6] {
        self.bars
    }

    fn capabilities(&self) -> Vec<CapDescriptor> {
        self.caps.clone()
    }

    fn config_read(&self, offset: u32, size: u32) -> Option<u32> {
        if offset as usize + size as usize > 0x1000 {
            return None;
        }
        Some(self.addr.read(offset as u16, size as u8))
    }

    fn config_write(&self, offset: u32, size: u32, word: u32) -> bool {
        if offset as usize + size as usize > 0x1000 {
            return false;
        }
        config_write(
            self.addr.seg,
            self.addr.bus,
            self.addr.slot,
            self.addr.func,
            offset as u16,
            size as u8,
            word,
        );
        true
    }

    fn capability_read(&self, index: i32, offset: u32, size: u32) -> Option<u32> {
        let cap = self.caps.get(index as usize)?;
        self.config_read(cap.offset as u32 + offset, size)
    }

    fn access_bar(&self, index: usize) -> hel::Result<hel::Handle> {
        let bar = self.bars.get(index).ok_or(hel::Error::IllegalArgs)?;
        match bar.host_type {
            IoType::Memory => {
                let aligned = (bar.address as usize) & !0xFFF;
                let page_off = (bar.address as usize) & 0xFFF;
                let span = ((bar.length as usize) + page_off + 0xFFF) & !0xFFF;
                hel::access_physical(aligned, span.max(0x1000))
            }
            IoType::Port => {
                let ports: Vec<usize> = (bar.address..bar.address + bar.length)
                    .map(|p| p as usize)
                    .collect();
                hel::access_io(&ports)
            }
            IoType::None => Err(hel::Error::IllegalArgs),
        }
    }

    fn access_irq(&self, _index: u64) -> hel::Result<Option<hel::Handle>> {
        match self.irq {
            Some(gsi) => Ok(Some(hel::access_irq(gsi as i32)?)),
            None => Ok(None),
        }
    }

    fn enable_busmaster(&self) {
        let command = self.addr.read16(0x04);
        config_write(
            self.addr.seg,
            self.addr.bus,
            self.addr.slot,
            self.addr.func,
            0x04,
            2,
            (command | 0x4) as u32,
        );
    }

    fn enable_irq(&self) {
        let command = self.addr.read16(0x04);
        config_write(
            self.addr.seg,
            self.addr.bus,
            self.addr.slot,
            self.addr.func,
            0x04,
            2,
            (command & !0x400) as u32,
        );
    }
}

fn string(value: &str) -> Item {
    Item::String(value.to_string())
}

fn hex(value: u32, width: usize) -> Item {
    Item::String(format!("{value:0width$x}"))
}

fn decimal(value: i64) -> Item {
    Item::String(format!("{value}"))
}

fn functions(seg: u16, bus: u8, slot: u8) -> Vec<u8> {
    let addr = Address {
        seg,
        bus,
        slot,
        func: 0,
    };
    if addr.read16(0x00) == 0xFFFF {
        return Vec::new();
    }
    if addr.read8(0x0E) & 0x80 != 0 {
        (0..8).collect()
    } else {
        vec![0]
    }
}

async fn serve_entity(manager: &'static EntityManager, device: Rc<SifPciDevice>) {
    let id = manager.id();

    loop {
        let (local, remote) = match hel::create_stream() {
            Ok(pair) => pair,
            Err(err) => {
                eprintln!("sif: entity {id}: create_stream failed: {err}");
                return;
            }
        };
        if let Err(err) = manager.serve_remote_lane(remote).await {
            eprintln!("sif: entity {id}: serve_remote_lane failed: {err}");
            return;
        }
        hel::spawn(serve_pci_device(local, device.clone()));
    }
}

fn device_properties(addr: Address, is_bridge: bool, parent_id: i64) -> Properties {
    let mut props = HashMap::new();
    props.insert("unix.subsystem".into(), string("pci"));
    props.insert(
        "pci-type".into(),
        string(if is_bridge {
            "pci-bridge"
        } else {
            "pci-device"
        }),
    );
    props.insert("pci-segment".into(), hex(addr.seg as u32, 4));
    props.insert("pci-bus".into(), hex(addr.bus as u32, 2));
    props.insert("pci-slot".into(), hex(addr.slot as u32, 2));
    props.insert("pci-function".into(), hex(addr.func as u32, 1));
    props.insert("pci-vendor".into(), hex(addr.read16(0x00) as u32, 4));
    props.insert("pci-device".into(), hex(addr.read16(0x02) as u32, 4));
    props.insert("pci-revision".into(), hex(addr.read8(0x08) as u32, 2));
    props.insert("pci-class".into(), hex(addr.read8(0x0B) as u32, 2));
    props.insert("pci-subclass".into(), hex(addr.read8(0x0A) as u32, 2));
    props.insert("pci-interface".into(), hex(addr.read8(0x09) as u32, 2));
    if !is_bridge {
        props.insert(
            "pci-subsystem-vendor".into(),
            hex(addr.read16(0x2C) as u32, 2),
        );
        props.insert(
            "pci-subsystem-device".into(),
            hex(addr.read16(0x2E) as u32, 2),
        );
    }
    props.insert("drvcore.mbus-parent".into(), decimal(parent_id));
    props
}

type Publication = (EntityManager, Option<Rc<SifPciDevice>>);

fn scan_bus<'a>(
    seg: u16,
    bus: u8,
    parent_id: i64,
    routes: &'a Routes,
    out: &'a mut Vec<Publication>,
) -> Pin<Box<dyn Future<Output = Result<()>> + 'a>> {
    Box::pin(async move {
        for slot in 0..32 {
            for func in functions(seg, bus, slot) {
                let addr = Address {
                    seg,
                    bus,
                    slot,
                    func,
                };
                if addr.read16(0x00) == 0xFFFF {
                    continue;
                }

                let is_bridge = addr.read8(0x0E) & 0x7F == 1;
                let props = device_properties(addr, is_bridge, parent_id);
                let name = if is_bridge {
                    "pci-bridge"
                } else {
                    "pci-device"
                };
                let irq = resolve_irq(addr, routes);
                println!(
                    "sif: pci: {seg:04x}:{bus:02x}:{slot:02x}.{func} {name} vendor: {:04x} device: {:04x} irq: {:?}",
                    addr.read16(0x00),
                    addr.read16(0x02),
                    irq,
                );
                let manager = create_entity(name, &props).await?;
                let id = manager.id();

                let device = Rc::new(SifPciDevice {
                    addr,
                    irq,
                    bars: read_bars(addr, if is_bridge { 2 } else { 6 }),
                    caps: read_capabilities(addr),
                });
                out.push((manager, Some(device)));

                if is_bridge {
                    let secondary = addr.read8(0x19);
                    if secondary > bus {
                        scan_bus(seg, secondary, id, routes, out).await?;
                    }
                }
            }
        }
        Ok(())
    })
}

#[derive(Clone, Copy)]
struct RootBus {
    seg: u16,
    bus: u8,
    node: *mut uacpi_namespace_node,
}

type Routes = HashMap<(u16, u8, u8), u32>;

unsafe extern "C" fn root_bus_callback(
    user: *mut c_void,
    node: *mut uacpi_namespace_node,
    _depth: uacpi_u32,
) -> uacpi_iteration_decision {
    let roots = unsafe { &mut *(user as *mut Vec<RootBus>) };

    let mut segment: uacpi_u64 = 0;
    unsafe { uacpi_sys::uacpi_eval_simple_integer(node, c"_SEG".as_ptr(), &mut segment) };
    let mut base_bus: uacpi_u64 = 0;
    unsafe { uacpi_sys::uacpi_eval_simple_integer(node, c"_BBN".as_ptr(), &mut base_bus) };

    roots.push(RootBus {
        seg: segment as u16,
        bus: base_bus as u8,
        node,
    });
    uacpi_sys::UACPI_ITERATION_DECISION_NEXT_PEER
}

fn find_root_buses() -> Vec<RootBus> {
    let mut roots: Vec<RootBus> = Vec::new();
    let user = &mut roots as *mut _ as *mut c_void;
    unsafe {
        uacpi_sys::uacpi_find_devices(c"PNP0A03".as_ptr(), Some(root_bus_callback), user);
        uacpi_sys::uacpi_find_devices(c"PNP0A08".as_ptr(), Some(root_bus_callback), user);
    }
    roots.sort_by_key(|root| (root.seg, root.bus));
    roots.dedup_by_key(|root| (root.seg, root.bus));
    if roots.is_empty() {
        roots.push(RootBus {
            seg: 0,
            bus: 0,
            node: std::ptr::null_mut(),
        });
    }
    roots
}

fn resolve_link(
    source: *mut uacpi_namespace_node,
    index: u32,
) -> Option<(u32, IrqTrigger, IrqPolarity)> {
    let mut raw_resources: *mut uacpi_sys::uacpi_resources = std::ptr::null_mut();
    let status = unsafe { uacpi_sys::uacpi_get_current_resources(source, &raw mut raw_resources) };
    if status != uacpi_sys::UACPI_STATUS_OK || raw_resources.is_null() {
        return None;
    }

    let resources =
        unsafe { std::slice::from_raw_parts((*raw_resources).entries, (*raw_resources).length) };

    let mut route = None;

    for cur in resources.iter() {
        let ty = cur.type_;
        if ty == uacpi_sys::UACPI_RESOURCE_TYPE_END_TAG as u32 {
            break;
        }
        if ty == uacpi_sys::UACPI_RESOURCE_TYPE_IRQ as u32 {
            let irq = unsafe { (*cur).__bindgen_anon_1.irq.as_ref() };
            if (index as usize) < irq.num_irqs as usize {
                let gsi = unsafe { *irq.irqs.as_ptr().add(index as usize) } as u32;
                route = Some((gsi, trigger_of(irq.triggering), polarity_of(irq.polarity)));
            }
            break;
        }
        if ty == uacpi_sys::UACPI_RESOURCE_TYPE_EXTENDED_IRQ as u32 {
            let irq = unsafe { (*cur).__bindgen_anon_1.extended_irq.as_ref() };
            if (index as usize) < irq.num_irqs as usize {
                let gsi = unsafe { *irq.irqs.as_ptr().add(index as usize) };
                route = Some((gsi, trigger_of(irq.triggering), polarity_of(irq.polarity)));
            }
            break;
        }
        let len = cur.length as usize;
        if len == 0 {
            break;
        }
    }

    unsafe { uacpi_sys::uacpi_free_resources(raw_resources) };
    route
}

fn trigger_of(triggering: u8) -> IrqTrigger {
    if triggering as u32 == uacpi_sys::UACPI_TRIGGERING_EDGE {
        IrqTrigger::Edge
    } else {
        IrqTrigger::Level
    }
}

fn polarity_of(polarity: u8) -> IrqPolarity {
    if polarity as u32 == uacpi_sys::UACPI_POLARITY_ACTIVE_HIGH {
        IrqPolarity::High
    } else {
        IrqPolarity::Low
    }
}

fn build_routes(roots: &[RootBus]) -> Routes {
    let mut routes = Routes::new();
    for root in roots {
        if root.node.is_null() {
            continue;
        }

        let mut table: *mut uacpi_sys::uacpi_pci_routing_table = std::ptr::null_mut();
        let status = unsafe { uacpi_sys::uacpi_get_pci_routing_table(root.node, &mut table) };
        if status != uacpi_sys::UACPI_STATUS_OK || table.is_null() {
            continue;
        }

        let num_entries = unsafe { (*table).num_entries };
        let entries = unsafe { (*table).entries.as_ptr() };
        for i in 0..num_entries {
            let entry = unsafe { &*entries.add(i) };
            let slot = (entry.address >> 16) as u8;
            let (gsi, trigger, polarity) = if entry.source.is_null() {
                (entry.index, IrqTrigger::Level, IrqPolarity::Low)
            } else {
                match resolve_link(entry.source, entry.index) {
                    Some(route) => route,
                    None => continue,
                }
            };

            if let Err(err) = hel::configure_irq(gsi as i32, trigger, polarity) {
                eprintln!("sif: Failed to configure GSI {gsi}: {err}");
                continue;
            }
            routes.insert((root.seg, slot, entry.pin), gsi);
        }

        unsafe { uacpi_sys::uacpi_free_pci_routing_table(table) };
    }
    routes
}

fn resolve_irq(addr: Address, routes: &Routes) -> Option<u32> {
    let pin = addr.read8(0x3D);
    if pin == 0 || pin > 4 {
        return None;
    }
    routes.get(&(addr.seg, addr.slot, pin - 1)).copied()
}

pub async fn publish_devices() -> Result<()> {
    let roots = find_root_buses();
    let routes = build_routes(&roots);
    println!("sif: Resolved {} PCI interrupt routes", routes.len());

    let mut entities: Vec<Publication> = Vec::new();

    for root in &roots {
        let seg = root.seg;
        let bus = root.bus;

        let mut props: Properties = HashMap::new();
        props.insert("unix.subsystem".into(), string("pci"));
        props.insert("pci-type".into(), string("pci-root-bus"));
        props.insert("pci-segment".into(), hex(seg as u32, 4));
        props.insert("pci-bus".into(), hex(bus as u32, 2));

        println!("sif: PCI root bus {seg:04x}:{bus:02x}");
        let manager = create_entity("pci-root-bus", &props).await?;
        let root_id = manager.id();
        entities.push((manager, None));

        scan_bus(seg, bus, root_id, &routes, &mut entities).await?;
    }

    println!("sif: Enumerated {} PCI objects", entities.len());

    for (manager, device) in entities {
        let manager: &'static EntityManager = Box::leak(Box::new(manager));
        if let Some(device) = device {
            hel::spawn(serve_entity(manager, device));
        }
    }

    Ok(())
}
