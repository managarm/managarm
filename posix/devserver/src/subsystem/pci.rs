//! The pci bus subsystem.

use std::sync::Arc;

use anyhow::{Result, bail};
use async_trait::async_trait;
use managarm::fs;
use managarm::hw;
use managarm::mbus;

use crate::device::{DeviceKey, DeviceSpec, Membership, Model, Parent};
use crate::subsystem::{
    Installed, hex_prop, observe, parse_prop, remote_lane, required_prop, string_prop,
};
use crate::sysfs::{Attribute, StaticAttribute};

const IORESOURCE_IO: u64 = 0x100;
const IORESOURCE_MEM: u64 = 0x200;

/// The first 256 bytes of configuration space, read on every access.
struct ConfigAttribute {
    hw: Arc<hw::Device>,
}

#[async_trait(?Send)]
impl Attribute for ConfigAttribute {
    fn size(&self) -> u64 {
        256
    }

    async fn show(&self) -> Result<Vec<u8>, fs::server::Error> {
        let mut data = Vec::with_capacity(256);
        for offset in (0..256).step_by(4) {
            let word = self.hw.load_pci_space(offset, 4).await.map_err(|e| {
                eprintln!("devserver: reading PCI configuration space failed: {e}");
                fs::server::Error::InternalError
            })?;
            data.extend_from_slice(&word.to_le_bytes());
        }
        Ok(data)
    }
}

/// The `resource` table: one "start end flags" line per BAR.
struct ResourceAttribute {
    hw: Arc<hw::Device>,
}

#[async_trait(?Send)]
impl Attribute for ResourceAttribute {
    async fn show(&self) -> Result<Vec<u8>, fs::server::Error> {
        let info = self.hw.get_pci_info().await.map_err(|e| {
            eprintln!("devserver: querying PCI BARs failed: {e}");
            fs::server::Error::InternalError
        })?;
        let mut out = String::new();
        for bar in info.bar_info() {
            let flags = match bar.host_type() {
                hw::pci::IoType::None => {
                    out += "0x0000000000000000 0x0000000000000000 0x0000000000000000\n";
                    continue;
                }
                hw::pci::IoType::Memory => IORESOURCE_MEM,
                hw::pci::IoType::Port => IORESOURCE_IO,
            };
            out += &format!(
                "0x{:016x} 0x{:016x} 0x{:016x}\n",
                bar.address(),
                bar.address() + bar.length() - 1,
                flags
            );
        }
        Ok(out.into_bytes())
    }
}

/// `resourceN`: mmap()able, backed by the BAR's memory object.
struct BarAttribute {
    hw: Arc<hw::Device>,
    index: usize,
    length: u64,
}

#[async_trait(?Send)]
impl Attribute for BarAttribute {
    fn writable(&self) -> bool {
        true
    }

    fn size(&self) -> u64 {
        self.length
    }

    async fn show(&self) -> Result<Vec<u8>, fs::server::Error> {
        Err(fs::server::Error::IllegalOperationTarget)
    }

    async fn access_memory(&self) -> Result<hel::Handle, fs::server::Error> {
        self.hw.access_bar(self.index).await.map_err(|e| {
            eprintln!("devserver: accessing PCI BAR {} failed: {e}", self.index);
            fs::server::Error::InternalError
        })
    }
}

async fn install_entity(
    model: Arc<Model>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let mbus_id = event.entity_id();
    let pci_type = required_prop(properties, "pci-type")?;
    let segment = required_prop(properties, "pci-segment")?;
    let bus = required_prop(properties, "pci-bus")?;

    match pci_type {
        "pci-root-bus" => {
            let spec = DeviceSpec::new(
                format!("pci{segment}:{bus}"),
                Parent::None,
                Membership::None,
            )
            .key(DeviceKey::primary(mbus_id));
            let device = model.create_and_announce(spec).await?;
            println!("devserver: Installed PCI root bus {}", device.name());
            Ok(vec![Box::new(device)])
        }
        "pci-device" | "pci-bridge" => {
            let slot = required_prop(properties, "pci-slot")?;
            let function = required_prop(properties, "pci-function")?;
            let parent_id: i64 = parse_prop(properties, "drvcore.mbus-parent")?;

            let vendor = hex_prop(properties, "pci-vendor")?;
            let device_id = hex_prop(properties, "pci-device")?;
            let class = hex_prop(properties, "pci-class")?;
            let subclass = hex_prop(properties, "pci-subclass")?;
            let progif = hex_prop(properties, "pci-interface")?;
            let owns_plainfb = string_prop(properties, "class") == Some("framebuffer");

            let bus_num = u32::from_str_radix(bus, 16)?;
            let slot_num = u32::from_str_radix(slot, 16)?;
            let function_num = u32::from_str_radix(function, 16)?;
            let name = format!("{segment}:{bus}:{slot}.{function}");
            println!(
                "devserver: Installing PCI {} {name} (mbus ID: {mbus_id})",
                if pci_type == "pci-device" {
                    "device"
                } else {
                    "bridge"
                }
            );

            let hw = Arc::new(hw::Device::new(remote_lane(&event).await?));

            let mut spec = DeviceSpec::new(
                name,
                Parent::Key(DeviceKey::primary(parent_id)),
                Membership::bus("pci"),
            )
            .key(DeviceKey::primary(mbus_id))
            .uevent_extra(
                "PCI_SLOT_NAME",
                format!("0000:{bus_num:02x}:{slot_num:02x}.{function_num:x}"),
            )
            .uevent_extra("PCI_CLASS", format!("{class:X}{subclass:02X}{progif:02X}"))
            .attr("vendor", StaticAttribute::new(format!("0x{vendor:04x}\n")))
            .attr(
                "device",
                StaticAttribute::new(format!("0x{device_id:04x}\n")),
            )
            .attr(
                "owns_plainfb",
                StaticAttribute::new(if owns_plainfb { "1" } else { "0" }),
            );
            if pci_type == "pci-device" {
                spec = spec
                    .attr(
                        "subsystem_vendor",
                        StaticAttribute::new(format!(
                            "0x{:04x}\n",
                            hex_prop(properties, "pci-subsystem-vendor")?
                        )),
                    )
                    .attr(
                        "subsystem_device",
                        StaticAttribute::new(format!(
                            "0x{:04x}\n",
                            hex_prop(properties, "pci-subsystem-device")?
                        )),
                    );
            }
            spec = spec
                .attr("config", Arc::new(ConfigAttribute { hw: hw.clone() }))
                .attr(
                    "class",
                    StaticAttribute::new(format!(
                        "0x{:06x}\n",
                        class << 16 | subclass << 8 | progif
                    )),
                )
                .attr("resource", Arc::new(ResourceAttribute { hw: hw.clone() }))
                // There is no sane way of resolving the IRQ line yet.
                .attr("irq", StaticAttribute::new("0\n"));

            let info = hw.get_pci_info().await?;
            for (index, bar) in info.bar_info().iter().enumerate() {
                if bar.host_type() == hw::pci::IoType::None {
                    continue;
                }
                spec = spec.attr(
                    &format!("resource{index}"),
                    Arc::new(BarAttribute {
                        hw: hw.clone(),
                        index,
                        length: bar.length() as u64,
                    }),
                );
            }

            Ok(vec![Box::new(model.create_and_announce(spec).await?)])
        }
        _ => bail!("unsupported PCI device type '{pci_type}'"),
    }
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    let subsystem = model
        .subsystem(&Membership::bus("pci"))?
        .expect("bus membership yields a subsystem");
    subsystem.dir.dir("slots")?;

    observe(
        mbus::Filter::Equals("unix.subsystem", "pci"),
        |scope, event| {
            let model = model.clone();
            scope.install(
                format!("PCI entity {}", event.entity_id()),
                install_entity(model, event),
            );
        },
    )
    .await
}
