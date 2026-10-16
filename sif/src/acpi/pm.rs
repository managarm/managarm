//! Powers off and restarts the system; port of thor's system/acpi/pm-interface.cpp and of the
//! power button handling of system/acpi/ec.cpp.

use std::collections::HashMap;
use std::ffi::CStr;
use std::sync::Arc;

use anyhow::Result;
use managarm::hw::Error as HwError;
use managarm::hw::server::{PmInterface, RebootCommand, serve_pm_interface};
use managarm::mbus::create_entity;

use crate::entity::{serve_entity_lanes, string};
use crate::leak;
use crate::uacpi::handlers::{self, FixedEvent};
use crate::uacpi::namespace::{self, IterationDecision, NamespaceNode};
use crate::uacpi::runtime::{self, Aml};
use crate::uacpi::sleep::{self, SleepState};

const HID_POWER_BUTTON: &CStr = c"PNP0C0C";

/// The Notify() value of a power button device that reports a press in S0.
const NOTIFY_POWER_BUTTON_PRESSED: u64 = 0x80;

/// Enters S5. Only returns if that failed.
fn power_off(aml: Aml) -> HwError {
    if let Err(err) = sleep::prepare_for_sleep_state(aml, SleepState::S5) {
        println!("sif: acpi: preparing to enter S5 failed: {err}");
    }
    if let Err(err) = sleep::enter_sleep_state(aml, SleepState::S5) {
        println!("sif: acpi: entering S5 failed: {err}");
    }
    HwError::DeviceError
}

#[cfg(target_arch = "x86_64")]
fn ps2_reset() -> hel::Result<()> {
    const PS2_COMMAND: usize = 0x64;
    const PS2_RESET: u8 = 0xFE;

    let window = crate::pio::PioWindow::new(PS2_COMMAND..PS2_COMMAND + 1)?;
    let space = window.enable()?;
    // SAFETY: the window only covers the command port of the PS/2 controller.
    unsafe { space.scalar_store::<u8>(0, PS2_RESET) };
    // 100ms should be long enough to actually reset.
    std::thread::sleep(std::time::Duration::from_millis(100));
    Ok(())
}

/// Resets the system. Only returns if that failed.
fn restart(aml: Aml) -> HwError {
    // Firmware may expect _PTS to run before a reset, which preparing for S5 does.
    if let Err(err) = sleep::prepare_for_sleep_state(aml, SleepState::S5) {
        println!("sif: acpi: preparing for reset failed: {err}");
    }
    if let Err(err) = sleep::reboot() {
        println!("sif: acpi: ACPI reset failed: {err}");
    }

    #[cfg(target_arch = "x86_64")]
    {
        if let Err(err) = ps2_reset() {
            println!("sif: acpi: cannot access the PS/2 controller: {err}");
        }
        println!("sif: acpi: reset using the PS/2 controller failed");
    }
    HwError::DeviceError
}

struct Pm;

impl PmInterface for Pm {
    async fn reboot(&self, command: RebootCommand) -> HwError {
        match command {
            RebootCommand::PowerOff => runtime::run(power_off).await,
            RebootCommand::Restart => runtime::run(restart).await,
        }
    }
}

/// Publishes the pm-interface entity, which serves reboot(2).
pub async fn publish() -> Result<()> {
    let mut props = HashMap::new();
    props.insert("class".into(), string("pm-interface"));

    let manager = leak(create_entity("pm-interface", &props).await?);
    let pm = Arc::new(Pm);
    hel::spawn(serve_entity_lanes(manager, move |lane| {
        hel::spawn(serve_pm_interface(lane, pm.clone()));
    }));

    Ok(())
}

fn find_power_buttons(aml: Aml) -> Result<Vec<NamespaceNode>> {
    let mut nodes = Vec::new();
    namespace::find_devices_at(aml, NamespaceNode::root(), &[HID_POWER_BUTTON], |node| {
        nodes.push(node);
        IterationDecision::Continue
    })?;
    Ok(nodes)
}

/// Powers off the system when the power button is pressed.
pub async fn init_power_button() -> Result<()> {
    let fixed = handlers::install_fixed_event_handler(FixedEvent::PowerButton, || {
        println!("sif: acpi: shutting down because of a power button press");
        // Entering S5 runs AML, which must not run in interrupt context.
        runtime::post(|aml| {
            power_off(aml);
        });
    })
    .await;
    if let Err(err) = fixed {
        println!("sif: acpi: failed to install the power button handler: {err}");
    }

    // Modern hardware uses power button devices instead of the fixed event.
    for node in runtime::run(find_power_buttons).await? {
        let installed = handlers::install_notify_handler(node, |_aml, _node, value| {
            if value != NOTIFY_POWER_BUTTON_PRESSED {
                println!("sif: acpi: ignoring power button Notify() value {value:#x}");
                return;
            }
            println!("sif: acpi: shutting down because of a power button notification");
            // Entering S5 takes the event lock, hence it cannot run on the Notify() thread.
            runtime::post(|aml| {
                power_off(aml);
            });
        })
        .await;
        if let Err(err) = installed {
            println!(
                "sif: acpi: failed to install the Notify() handler of power button {}: {err}",
                node.absolute_path()
            );
        }
    }

    Ok(())
}
