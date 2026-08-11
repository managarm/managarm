use std::collections::HashMap;
use std::ffi::c_void;
use std::sync::OnceLock;

use hel::{IrqPolarity, IrqTrigger};

use uacpi_sys::{uacpi_iteration_decision, uacpi_namespace_node, uacpi_u32, uacpi_u64};

use super::discover::add_root_bus;
use super::{IrqIndex, PciBus, config};

#[derive(Clone, Copy)]
struct RootBus {
    seg: u16,
    bus: u8,
    node: *mut uacpi_namespace_node,
}

type Routes = HashMap<(u16, u8, u8), u32>;

static ROUTES: OnceLock<Routes> = OnceLock::new();

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
                println!("sif: Failed to configure GSI {gsi}: {err}");
                continue;
            }
            routes.insert((root.seg, slot, entry.pin), gsi);
        }

        unsafe { uacpi_sys::uacpi_free_pci_routing_table(table) };
    }
    routes
}

pub fn resolve_irq(seg: u16, slot: u8, index: IrqIndex) -> Option<u32> {
    ROUTES.get()?.get(&(seg, slot, index as u8 - 1)).copied()
}

pub fn discover_root_buses() {
    let roots = find_root_buses();

    let routes = build_routes(&roots);
    println!("sif: Resolved {} PCI interrupt routes", routes.len());
    ROUTES
        .set(routes)
        .expect("sif: PCI interrupt routes were already resolved");

    for root in roots {
        let Some(io) = config::get_config_io_for(root.seg, root.bus) else {
            println!(
                "sif: No config space for PCI host bridge {:04x}:{:02x}",
                root.seg, root.bus
            );
            continue;
        };

        println!(
            "sif: Found PCI host bridge {:04x}:{:02x}",
            root.seg, root.bus
        );

        let root_bus = PciBus::new(None, io, root.seg, root.bus);
        add_root_bus(root_bus);
    }
}
