//! /sys/firmware/dmi: the SMBIOS entry point and table as binary attributes.

use std::sync::Arc;

use anyhow::Result;
use managarm::hw;
use managarm::mbus;

use crate::device::Model;
use crate::subsystem::{Installed, observe, remote_lane, string_prop};
use crate::sysfs::StaticAttribute;

async fn install_tables(
    model: Arc<Model>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let hw = hw::Device::new(remote_lane(&event).await?);
    let header = hw.get_smbios_header().await?;
    let table = hw.get_smbios_table().await?;

    let tables = model.firmware_dir.dir("dmi")?.dir("tables")?;
    tables.create_attr("smbios_entry_point", StaticAttribute::new_sized(header))?;
    tables.create_attr("DMI", StaticAttribute::new_sized(table))?;
    // The tables are not a device.
    Ok(Vec::new())
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    let filters = [
        mbus::Filter::Equals("unix.subsystem", "firmware"),
        mbus::Filter::Equals("firmware.type", "smbios"),
    ];
    let mut installed = false;
    observe(mbus::Filter::Conjunction(&filters), |scope, event| {
        // Only SMBIOS 3 entry points are exposed, and only one of them.
        if installed || string_prop(event.properties(), "version") != Some("3") {
            return;
        }
        installed = true;
        scope.install(
            "SMBIOS tables".to_string(),
            install_tables(model.clone(), event),
        );
    })
    .await
}
