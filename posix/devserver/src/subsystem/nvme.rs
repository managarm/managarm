//! The nvme subsystems: NVMe subsystems, controllers and namespaces, plus the
//! static nvme-fabrics control device.

use std::sync::{Arc, Mutex};

use anyhow::{Result, bail};
use managarm::mbus;

use crate::EXPECT_LOCK;
use crate::device::{
    AttrGroup, Device, DeviceKey, DeviceSpec, IdAllocator, Membership, Model, Parent,
};
use crate::subsystem::{
    Installed, attr, mbus_parent, observe, parse_prop, required_prop, string_prop,
};
use crate::sysfs::StaticAttribute;

struct State {
    model: Arc<Model>,
    fabrics_ctl: Arc<Device>,
    subsystems: Mutex<IdAllocator>,
    controllers: Mutex<IdAllocator>,
}

fn nqn(subsystem: &str) -> String {
    format!("nqn.2014-08.org.nvmexpress:nvm-subsystem:{subsystem}\n")
}

async fn install_subsystem(
    state: Arc<State>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let name = format!(
        "nvme-subsys{}",
        state.subsystems.lock().expect(EXPECT_LOCK).allocate()
    );
    let spec = DeviceSpec::new(
        name.clone(),
        Parent::None,
        Membership::class("nvme-subsystem"),
    )
    .key(DeviceKey::primary(event.entity_id()));
    let attrs = vec![
        attr("subsysnqn", nqn(&name)),
        attr("subsystype", "nvm\n"),
        attr("iopolicy", "numa\n"),
    ];
    let device = state.model.create_and_announce(spec.attrs(attrs)).await?;
    println!(
        "devserver: installed {name} (mbus ID {})",
        event.entity_id()
    );
    Ok(vec![Box::new(device)])
}

async fn install_controller(
    state: Arc<State>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let subsystem_id: i64 = parse_prop(properties, "nvme.subsystem")?;
    let address = required_prop(properties, "nvme.address")?;
    let transport = required_prop(properties, "nvme.transport")?;
    let model_name = required_prop(properties, "nvme.model")?;
    let serial = required_prop(properties, "nvme.serial")?;
    let fw_rev = required_prop(properties, "nvme.fw-rev")?;

    let subsystem = state
        .model
        .wait_device(&DeviceKey::primary(subsystem_id))
        .await;
    // Fabrics controllers have no bus parent and hang off the control device.
    let parent = match mbus_parent(properties) {
        Some(id) => Parent::Key(DeviceKey::primary(id)),
        None if transport == "tcp" => Parent::Device(state.fabrics_ctl.clone()),
        None => Parent::None,
    };

    let name = format!(
        "nvme{}",
        state.controllers.lock().expect(EXPECT_LOCK).allocate()
    );
    let spec = DeviceSpec::new(name.clone(), parent, Membership::class("nvme"))
        .key(DeviceKey::primary(event.entity_id()));
    let attrs = vec![
        attr("subsysnqn", nqn(subsystem.name())),
        attr("transport", format!("{transport}\n")),
        attr("address", format!("{address}\n")),
        attr("state", "live\n"),
        attr("cntlid", "2\n"),
        attr("cntrltype", "io\n"),
        attr("numa_node", "-1\n"),
        attr("serial", format!("{serial}\n")),
        attr("model", format!("{model_name}\n")),
        attr("firmware_rev", format!("{fw_rev}\n")),
    ];
    let device = state.model.create_and_announce(spec.attrs(attrs)).await?;
    let relation = state.model.relate(&subsystem, &name, &device, None)?;
    println!(
        "devserver: installed {name} (mbus ID {})",
        event.entity_id()
    );
    Ok(vec![Box::new(device), Box::new(relation)])
}

async fn install_namespace(
    state: Arc<State>,
    event: mbus::EnumerationEvent,
) -> Result<Vec<Box<dyn Installed>>> {
    let properties = event.properties();
    let nsid: u64 = parse_prop(properties, "nvme.nsid")?;
    let Some(parent_id) = mbus_parent(properties) else {
        bail!("NVMe namespace has no controller");
    };
    let controller = state
        .model
        .wait_device(&DeviceKey::primary(parent_id))
        .await;

    let name = format!("{}n{nsid}", controller.name());
    // TODO: Namespaces are block devices on Linux; make them Membership::Block.
    let spec = DeviceSpec::new(name.clone(), Parent::Device(controller), Membership::None)
        .key(DeviceKey::primary(event.entity_id()))
        .attrs(vec![attr("nsid", format!("{nsid}\n")), attr("size", "0\n")])
        .group(AttrGroup::named("queue").attr("logical_block_size", StaticAttribute::new("512\n")));
    let device = state.model.create_and_announce(spec).await?;
    println!(
        "devserver: installed {name} (mbus ID {})",
        event.entity_id()
    );
    Ok(vec![Box::new(device)])
}

pub async fn run(model: Arc<Model>) -> Result<()> {
    // The class directory exists even before the first device shows up.
    model.subsystem(&Membership::class("nvme"))?;
    model.subsystem(&Membership::class("nvme-subsystem"))?;
    let fabrics_ctl = model
        .create_and_announce(DeviceSpec::new(
            "ctl",
            Parent::None,
            Membership::class("nvme-fabrics"),
        ))
        .await?;
    let fabrics_ctl_device = fabrics_ctl.device().clone();
    fabrics_ctl.persist();
    let state = Arc::new(State {
        model,
        fabrics_ctl: fabrics_ctl_device,
        subsystems: Mutex::new(IdAllocator::new(0)),
        controllers: Mutex::new(IdAllocator::new(0)),
    });

    let filters = [
        mbus::Filter::Equals("class", "nvme-subsystem"),
        mbus::Filter::Equals("class", "nvme-controller"),
        mbus::Filter::Equals("class", "nvme-namespace"),
    ];
    observe(mbus::Filter::Disjunction(&filters), |scope, event| {
        let what = format!("NVMe entity {}", event.entity_id());
        let class = string_prop(event.properties(), "class")
            .unwrap_or("")
            .to_string();
        match class.as_str() {
            "nvme-subsystem" => scope.install(what, install_subsystem(state.clone(), event)),
            "nvme-controller" => scope.install(what, install_controller(state.clone(), event)),
            "nvme-namespace" => scope.install(what, install_namespace(state.clone(), event)),
            other => println!(
                "devserver: unsupported NVMe device class '{other}' (mbus ID {})",
                event.entity_id()
            ),
        }
    })
    .await
}
