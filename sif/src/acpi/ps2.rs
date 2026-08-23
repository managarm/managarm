//! Publishes the PS/2 devices of the ACPI namespace; port of thor's system/acpi/ps2.cpp.

use std::collections::{BTreeMap, HashMap};
use std::ffi::CStr;
use std::sync::{Arc, Mutex};

use anyhow::Result;
use managarm::hw::Error as HwError;
use managarm::hw::server::{AcpiObject, AcpiResources, serve_acpi_object};
use managarm::mbus::create_entity;
use managarm::svrctl::hardware_access_handle;

use crate::entity::{serve_entity_lanes, string};
use crate::leak;
use crate::uacpi::namespace::{self, IterationDecision, NamespaceNode};
use crate::uacpi::resources::Resource;

const ACPI_HID_PS2_KEYBOARDS: &[&CStr] = &[
    c"PNP0300", c"PNP0301", c"PNP0302", c"PNP0303", c"PNP0304", c"PNP0305", c"PNP0306", c"PNP0307",
    c"PNP0308", c"PNP0309", c"PNP030A", c"PNP030B", c"PNP0320", c"PNP0321", c"PNP0322", c"PNP0323",
    c"PNP0324", c"PNP0325", c"PNP0326", c"PNP0327", c"PNP0340", c"PNP0341", c"PNP0342", c"PNP0343",
    c"PNP0344",
];

const ACPI_HID_PS2_MICE: &[&CStr] = &[
    c"PNP0F00", c"PNP0F01", c"PNP0F02", c"PNP0F03", c"PNP0F04", c"PNP0F05", c"PNP0F06", c"PNP0F07",
    c"PNP0F08", c"PNP0F09", c"PNP0F0A", c"PNP0F0B", c"PNP0F0C", c"PNP0F0D", c"PNP0F0E", c"PNP0F0F",
    c"PNP0F10", c"PNP0F11", c"PNP0F12", c"PNP0F13", c"PNP0F14", c"PNP0F15", c"PNP0F16", c"PNP0F17",
    c"PNP0F18", c"PNP0F19", c"PNP0F1A", c"PNP0F1B", c"PNP0F1C", c"PNP0F1D", c"PNP0F1E", c"PNP0F1F",
    c"PNP0F20", c"PNP0F21", c"PNP0F22", c"PNP0F23", c"PNP0FFC", c"PNP0FFF",
];

// The IRQ object map is only locked for the duration of a single operation, none of which can panic.
const EXPECT_LOCK: &str = "sif: ACPI IRQ object mutex was poisoned";

struct Ps2Object {
    node: NamespaceNode,
    // Created on demand but shared between requests, as thor has one IRQ object per interrupt.
    irq_objects: Mutex<BTreeMap<usize, &'static hel::Handle>>,
}

/// Returns the individual ports of a port resource, or None for other resource types.
fn port_list(resource: &Resource) -> Option<Vec<u16>> {
    match resource {
        Resource::Io(io) => Some((io.minimum()..=io.maximum()).collect()),
        Resource::FixedIo(io) => Some(
            (0..u16::from(io.length()))
                .map(|i| io.address() + i)
                .collect(),
        ),
        _ => None,
    }
}

/// Returns the IRQs of an IRQ resource, or None for other resource types.
fn irq_list(resource: &Resource) -> Option<Vec<u8>> {
    match resource {
        Resource::Irq(irq) => Some(irq.irqs().to_vec()),
        Resource::ExtendedIrq(irq) => Some(irq.irqs().iter().map(|&i| i as u8).collect()),
        _ => None,
    }
}

/// Resolves an IRQ of _CRS to the GSI that raises it.
fn resolve_irq_to_gsi(irq: u8) -> u32 {
    #[cfg(target_arch = "x86_64")]
    {
        crate::isa::resolve_isa_irq(irq).gsi
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        u32::from(irq)
    }
}

impl AcpiObject for Ps2Object {
    fn resources(&self) -> Option<AcpiResources> {
        let resources = self.node.current_resources().ok()?;

        let mut out = AcpiResources::default();
        for resource in resources.iter() {
            match &resource {
                Resource::Io(io) => out.io_ports.extend(io.minimum()..=io.maximum()),
                Resource::FixedIo(io) => out
                    .fixed_io_ports
                    .extend((0..u16::from(io.length())).map(|i| io.address() + i)),
                Resource::Other(type_) => {
                    println!("sif: acpi: Ignoring _CRS resource of type {type_}")
                }
                // IRQ resources are collected below.
                Resource::Irq(_) | Resource::ExtendedIrq(_) => (),
            }
            if let Some(irqs) = irq_list(&resource) {
                out.irqs.extend(irqs);
            }
        }
        Some(out)
    }

    fn access_ports(&self, index: usize) -> managarm::hw::Result<hel::Handle> {
        let resources = self
            .node
            .current_resources()
            .map_err(|_| HwError::DeviceError)?;

        let mut i = 0;
        for resource in resources.iter() {
            let Some(ports) = port_list(&resource) else {
                continue;
            };
            if i == index {
                let ports: Vec<usize> = ports.iter().map(|&port| usize::from(port)).collect();
                return Ok(hel::access_io(hardware_access_handle(), &ports)?);
            }
            i += 1;
        }
        Err(HwError::OutOfBounds)
    }

    fn access_irq(&self, index: usize) -> managarm::hw::Result<&hel::Handle> {
        let mut objects = self.irq_objects.lock().expect(EXPECT_LOCK);
        if let Some(&object) = objects.get(&index) {
            return Ok(object);
        }

        let resources = self
            .node
            .current_resources()
            .map_err(|_| HwError::DeviceError)?;

        let mut irqs = Vec::new();
        for resource in resources.iter() {
            if let Some(list) = irq_list(&resource) {
                irqs.extend(list);
            }
        }

        let &irq = irqs.get(index).ok_or(HwError::OutOfBounds)?;
        let gsi = resolve_irq_to_gsi(irq);
        let pin = hel::access_irq_by_gsi(hardware_access_handle(), u64::from(gsi))?;
        let object = leak(hel::handle_irq(&pin)?);
        objects.insert(index, object);
        Ok(object)
    }
}

async fn publish_object(node: NamespaceNode, instance: usize) -> Result<()> {
    let path = node.absolute_path();

    let mut props = HashMap::new();
    props.insert("unix.subsystem".into(), string("acpi"));
    props.insert("acpi.path".into(), string(&path));
    if let Some(hid) = node.eval_hid().ok().flatten() {
        props.insert("acpi.hid".into(), string(&hid));
    }
    if let Some(cid) = node
        .eval_cid()
        .ok()
        .flatten()
        .and_then(|ids| ids.into_iter().next())
    {
        props.insert("acpi.cid".into(), string(&cid));
    }
    props.insert("acpi.instance".into(), string(&instance.to_string()));

    println!("sif: acpi: publishing object {path}");
    let manager = leak(create_entity("acpi-object", &props).await?);
    let object = Arc::new(Ps2Object {
        node,
        irq_objects: Mutex::new(BTreeMap::new()),
    });
    hel::spawn(serve_entity_lanes(manager, move |lane| {
        hel::spawn(serve_acpi_object(lane, object.clone()));
    }));

    Ok(())
}

async fn publish_devices(hids: &[&CStr]) -> Result<()> {
    let mut nodes = Vec::new();
    namespace::find_devices_at(NamespaceNode::root(), hids, |node| {
        nodes.push(node);
        IterationDecision::Continue
    })?;

    for (instance, node) in nodes.into_iter().enumerate() {
        publish_object(node, instance).await?;
    }
    Ok(())
}

/// Publishes the acpi-object entities of all PS/2 keyboards and mice.
pub async fn publish() -> Result<()> {
    publish_devices(ACPI_HID_PS2_KEYBOARDS).await?;
    publish_devices(ACPI_HID_PS2_MICE).await?;

    Ok(())
}
