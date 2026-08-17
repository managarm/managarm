use std::collections::BTreeMap;
use std::sync::Mutex;

use hel::{IrqPolarity, IrqTrigger};
use managarm::svrctl::hardware_access_handle;

use crate::dt::node::{DeviceTreeNode, get_device_tree_root, walk_interrupt_map};

use super::config::PciConfigIo;
use super::config::ecam::EcamPcieConfigIo;
use super::discover::add_root_bus;
use super::{
    EXPECT_LOCK, IrqIndex, IrqPin, PciBus, PciBusResource, PciIrqRouter, RouterState, RoutingEntry,
    RoutingModel, leak,
};

const LOG_ROUTING_TABLE: bool = false;

static DT_PCI_COMPATIBLE: [&str; 3] = [
    "pci-host-cam-generic",
    "pci-host-ecam-generic",
    "brcm,bcm2711-pcie",
];

static DT_IRQ_PINS: Mutex<BTreeMap<(u32, u64), &'static IrqPin>> = Mutex::new(BTreeMap::new());

// Configures a DT interrupt and returns its pin, sharing pins between users of the same
// (controller, index) pair.
fn dt_irq(
    controller: &'static DeviceTreeNode,
    index: u64,
    trigger: Option<IrqTrigger>,
    polarity: Option<IrqPolarity>,
) -> Option<&'static IrqPin> {
    let phandle = controller.phandle();
    let mut pins = DT_IRQ_PINS.lock().expect(EXPECT_LOCK);
    if let Some(pin) = pins.get(&(phandle, index)) {
        if pin.trigger != trigger || pin.polarity != polarity {
            println!(
                "sif: Conflicting configurations for IRQ {index} of {}",
                controller.path()
            );
        }
        return Some(*pin);
    }

    let handle = match hel::access_irq_by_phandle(hardware_access_handle(), phandle.into(), index) {
        Ok(handle) => handle,
        Err(err) => {
            println!(
                "sif: Failed to access IRQ {index} of {}: {err}",
                controller.path()
            );
            return None;
        }
    };
    if let Err(err) = hel::configure_irq(&handle, trigger, polarity) {
        println!(
            "sif: Failed to configure IRQ {index} of {}: {err}",
            controller.path()
        );
        return None;
    }

    let pin = leak(IrqPin {
        name: format!("{}:{index}", controller.name()),
        handle,
        trigger,
        polarity,
    });
    pins.insert((phandle, index), pin);
    Some(pin)
}

pub struct DtbPciIrqRouter {
    state: RouterState,
}

impl DtbPciIrqRouter {
    pub fn new(
        parent: Option<&'static dyn PciIrqRouter>,
        bus: &'static PciBus,
        node: Option<&'static DeviceTreeNode>,
    ) -> &'static DtbPciIrqRouter {
        let mut state = RouterState::new();
        build_routing(&mut state, parent, bus, node);

        leak(DtbPciIrqRouter { state })
    }
}

fn build_routing(
    state: &mut RouterState,
    parent: Option<&'static dyn PciIrqRouter>,
    bus: &'static PciBus,
    node: Option<&'static DeviceTreeNode>,
) {
    let Some(node) = node else {
        let parent = parent.expect("expansion bridge routing without a parent router");
        let bridge = bus
            .associated_bridge
            .expect("expansion bridge routing without an associated bridge");

        for (i, bridge_irq) in state.bridge_irqs.iter_mut().enumerate() {
            *bridge_irq =
                parent.resolve_irq_route(bridge.entity.slot, IrqIndex::from_pin(i as u8 + 1));
            if let Some(pin) = bridge_irq {
                println!("sif:     Bridge IRQ [{i}]: {}", pin.name());
            }
        }

        state.routing_model = RoutingModel::ExpansionBridge;
        return;
    };

    let Some(mask_prop) = node.dt_node().find_property("interrupt-map-mask") else {
        panic!("{} has no interrupt-map-mask", node.path());
    };

    let mask = mask_prop
        .access()
        .read_cells(1)
        .unwrap_or_else(|| panic!("{}: failed to read interrupt-map-mask field", node.path()))
        as u32;

    // TODO(qookie): We mask off the function bits here.
    let ignored = !mask & 0x0000F800;
    let n_ignored_comb = 1usize << ignored.count_ones();

    // This bit of code maps values in [0, n_ignored_comb)
    // onto bits that are set in ignored.
    for i in 0..n_ignored_comb {
        let mut disp: u32 = 0;
        let mut n = 0;
        for j in 0..16 {
            if ignored & (1 << j) != 0 {
                disp |= (((i >> n) & 1) as u32) << j;
                n += 1;
            }
        }

        let success = walk_interrupt_map(
            &mut |child_address, child_irq, parent_node, _parent_address, parent_irq| {
                if child_address.num_cells() != 3 {
                    panic!("Expected three child address cells in ECAM interrupt-map");
                }
                let bdf = child_address
                    .read_slice(0, 1)
                    .expect("Failed to read BDF from ECAM interupt-map")
                    as u32;
                let addr = bdf + disp;
                let bus_id = (addr >> 16) & 0xFF;
                let slot = (addr >> 11) & 0x1F;
                let func = (addr >> 8) & 0x07;
                assert!(bus_id == bus.bus_id as u32);
                assert!(func == 0, "TODO: support routing of individual functions");

                let index = child_irq
                    .read()
                    .expect("Failed to read pin index from interrupt-map");
                // The parent address does not matter in this case
                // (and is not present on QEMU's virt machine on AArch64).

                let irq_controller = parent_node.associated_irq_controller().unwrap_or_else(|| {
                    panic!("No IRQ controller associated with {}", parent_node.path())
                });
                let Some(irq) = irq_controller.resolve_dt_irq(parent_irq) else {
                    return;
                };
                let Some(pin) = dt_irq(parent_node, irq.index, irq.trigger, irq.polarity) else {
                    return;
                };
                if LOG_ROUTING_TABLE {
                    println!(
                        "sif: {bus_id} {slot} [{index}]: Routed to IRQ {}",
                        pin.name()
                    );
                }
                state.routing_table.push(RoutingEntry {
                    slot: slot as u8,
                    index: IrqIndex::from_pin(index as u8),
                    pin,
                });
            },
            node,
        );
        if !success {
            panic!("Failed to walk interrupt-map of {}", node.path());
        }
    }

    state.routing_model = RoutingModel::RootTable;
}

impl PciIrqRouter for DtbPciIrqRouter {
    fn resolve_irq_route(&self, slot: u8, index: IrqIndex) -> Option<&'static IrqPin> {
        self.state.resolve_irq_route(slot, index)
    }

    fn make_downstream_router(&'static self, bus: &'static PciBus) -> &'static dyn PciIrqRouter {
        DtbPciIrqRouter::new(Some(self), bus, None)
    }
}

fn init_pci_node(node: &'static DeviceTreeNode) {
    println!("sif: Initializing node \"{}\":", node.path());

    let range = node.bus_range();

    let io: &'static dyn PciConfigIo = if node.is_compatible(&["pci-host-ecam-generic"]) {
        println!("sif:     It's a generic controller with ECAM IO.");
        assert!(node.reg().len() == 1);

        leak(EcamPcieConfigIo::new(
            node.reg()[0].addr,
            0,
            range.from as u8,
            range.to as u8,
        ))
    } else {
        // The Broadcom STB PCIe controller (brcm,bcm2711-pcie) is not ported yet.
        println!("sif: Unsupported PCI(e) controller \"{}\"", node.path());
        return;
    };

    let root_bus = PciBus::new(None, io, 0, range.from as u8);
    let router = DtbPciIrqRouter::new(None, root_bus, Some(node));
    assert!(
        root_bus.irq_router.set(router).is_ok(),
        "sif: PCI bus already has an IRQ router"
    );

    for r in node.ranges() {
        assert!(r.child_addr_hi_valid);

        let type_ = (r.child_addr_hi >> 24) & 0b11;

        let mut res_flags = 0;

        match type_ {
            1 => res_flags = PciBusResource::IO,
            2 | 3 => {
                res_flags = PciBusResource::MEMORY;

                if r.child_addr_hi & (1 << 30) != 0 {
                    res_flags = PciBusResource::PREF_MEMORY;
                }
            }
            _ => println!("sif: Unexpected range type {type_}"),
        }

        println!(
            "sif: Adding resource {:#x} with flags {res_flags}",
            r.child_addr
        );

        root_bus
            .resources
            .lock()
            .expect(EXPECT_LOCK)
            .push(PciBusResource::new(
                r.child_addr,
                r.size,
                r.parent_addr,
                res_flags,
                true,
            ));
    }

    add_root_bus(root_bus);
}

pub fn discover_root_buses() {
    let Some(root) = get_device_tree_root() else {
        return;
    };

    let mut i = 0;

    root.for_each(&mut |node| {
        if node.is_compatible(&DT_PCI_COMPATIBLE) {
            init_pci_node(node);
            i += 1;
        }

        false
    });

    println!("sif: Found {i} PCI nodes in total.");
}
