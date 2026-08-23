//! Publishes dt-node entities for the device tree; port of thor's system/dtb/dtb_discover.cpp.

use std::collections::HashMap;
use std::sync::{Arc, OnceLock};

use anyhow::Result;
use managarm::hw::Error as HwError;
use managarm::hw::server::{DtNode, DtRegisterDescriptor, serve_dt_node};
use managarm::mbus::create_entity;
use managarm::svrctl::hardware_access_handle;

use crate::acpi::{PAGE_MASK, PAGE_SIZE};
use crate::entity::{decimal, serve_entity_lanes, string};
use crate::irq::{IrqPin, dt_irq};
use crate::leak;

use super::fdt::Cells;
use super::node::{DeviceTreeNode, get_device_tree_root, walk_interrupts};

struct DtRegister {
    address: u64,
    length: u64,
    offset: u32,
}

struct DtIrq {
    parent: &'static DeviceTreeNode,
    cells: Cells<'static>,
    // Created on demand but shared between requests, as thor has one IRQ object per interrupt.
    object: OnceLock<hel::Handle>,
}

impl DtIrq {
    fn pin(&self) -> managarm::hw::Result<&'static IrqPin> {
        let controller = self
            .parent
            .associated_irq_controller()
            .ok_or(HwError::DeviceError)?;
        let irq = controller
            .resolve_dt_irq(self.cells)
            .ok_or(HwError::DeviceError)?;
        dt_irq(self.parent, irq.index, irq.trigger, irq.polarity).ok_or(HwError::DeviceError)
    }

    fn object(&self) -> managarm::hw::Result<&hel::Handle> {
        if let Some(object) = self.object.get() {
            return Ok(object);
        }
        let object = hel::handle_irq(self.pin()?.handle())?;
        Ok(self.object.get_or_init(|| object))
    }
}

struct ServedDtNode {
    node: &'static DeviceTreeNode,
    regs: Vec<DtRegister>,
    irqs: Vec<DtIrq>,
}

impl ServedDtNode {
    fn new(node: &'static DeviceTreeNode) -> ServedDtNode {
        let regs = node
            .reg()
            .iter()
            .map(|reg| DtRegister {
                address: reg.addr,
                length: reg.size,
                offset: (reg.addr as usize & PAGE_MASK) as u32,
            })
            .collect();

        let mut irqs = Vec::new();
        let mut collect = |parent, cells| {
            irqs.push(DtIrq {
                parent,
                cells,
                object: OnceLock::new(),
            });
        };
        if walk_interrupts(&mut collect, node) == Some(false) {
            println!(
                "sif: {}: failed to parse interrupts for mbus node",
                node.path()
            );
        }
        // TODO(qookie): Try interrupts-extended if interrupts failed.

        ServedDtNode { node, regs, irqs }
    }
}

impl DtNode for ServedDtNode {
    fn regs(&self) -> Vec<DtRegisterDescriptor> {
        self.regs
            .iter()
            .map(|reg| DtRegisterDescriptor {
                address: reg.address,
                length: reg.length,
                offset: reg.offset,
            })
            .collect()
    }

    fn num_irqs(&self) -> u32 {
        self.irqs.len() as u32
    }

    fn path(&self) -> String {
        self.node.path().to_string()
    }

    fn property(&self, name: &str) -> Option<Vec<u8>> {
        self.node
            .dt_node()
            .find_property(name)
            .map(|prop| prop.data().to_vec())
    }

    fn properties(&self) -> Vec<(String, Vec<u8>)> {
        self.node
            .dt_node()
            .properties()
            .map(|prop| (prop.name().to_string(), prop.data().to_vec()))
            .collect()
    }

    fn access_register(&self, index: usize) -> managarm::hw::Result<hel::Handle> {
        let reg = self.regs.get(index).ok_or(HwError::OutOfBounds)?;
        let aligned = (reg.address as usize) & !PAGE_MASK;
        let page_off = (reg.address as usize) & PAGE_MASK;
        let span = ((reg.length as usize) + page_off + PAGE_MASK) & !PAGE_MASK;
        Ok(hel::access_physical(
            hardware_access_handle(),
            aligned,
            span.max(PAGE_SIZE),
            hel::CachingMode::MmioNonPosted,
        )?)
    }

    fn install_irq(&self, index: usize) -> managarm::hw::Result<&hel::Handle> {
        self.irqs.get(index).ok_or(HwError::OutOfBounds)?.object()
    }

    fn enable_irqs(&self) {
        for (index, irq) in self.irqs.iter().enumerate() {
            if irq.object().is_err() {
                println!("sif: {}: failed to configure IRQ {index}", self.node.path());
            }
        }
    }
}

async fn publish_node(node: &'static DeviceTreeNode, parent: Option<i64>) -> Result<i64> {
    let mut props = HashMap::new();
    props.insert("unix.subsystem".into(), string("dt"));
    if let Some(parent) = parent {
        props.insert("drvcore.mbus-parent".into(), decimal(parent));
    }
    // mbus filters can only match on keys, hence the value is part of the key.
    for compatible in node.compatible() {
        props.insert(format!("dt.compatible={compatible}"), string(""));
    }

    let manager = leak(create_entity("dt-node", &props).await?);
    let served = Arc::new(ServedDtNode::new(node));
    hel::spawn(serve_entity_lanes(manager, move |lane| {
        hel::spawn(serve_dt_node(lane, served.clone()));
    }));

    Ok(manager.id())
}

/// Publishes a dt-node entity for every node of the device tree.
pub async fn publish_all() -> Result<()> {
    let Some(root) = get_device_tree_root() else {
        return Ok(());
    };

    // Collect in pre-order so that parents are published before their children.
    let mut nodes = Vec::new();
    root.for_each(&mut |node| {
        if !node.name().starts_with("memory@") {
            nodes.push(node);
        }
        false
    });
    println!("sif: Found {} DT nodes in total.", nodes.len());

    let mut mbus_ids: HashMap<*const DeviceTreeNode, i64> = HashMap::new();
    for node in nodes {
        let parent = node
            .parent()
            .and_then(|parent| mbus_ids.get(&std::ptr::from_ref(parent)).copied());
        let id = publish_node(node, parent).await?;
        mbus_ids.insert(std::ptr::from_ref(node), id);
    }

    Ok(())
}
