//! IRQ pins accessed from the kernel, shared between the PCI and DT subsystems.

use std::collections::BTreeMap;
use std::sync::Mutex;

use hel::{IrqPolarity, IrqTrigger};
use managarm::svrctl::hardware_access_handle;

use crate::dt::node::DeviceTreeNode;
use crate::leak;

// The pin maps are only locked for the duration of a single operation, none of which can panic.
const EXPECT_LOCK: &str = "sif: IRQ pin map mutex was poisoned";

pub struct IrqPin {
    name: String,
    handle: hel::Handle,
    // None if the interrupt controller has no configurable trigger mode / polarity.
    trigger: Option<IrqTrigger>,
    polarity: Option<IrqPolarity>,
}

impl IrqPin {
    pub fn name(&self) -> &str {
        &self.name
    }

    pub fn handle(&self) -> &hel::Handle {
        &self.handle
    }
}

static IRQ_PINS: Mutex<BTreeMap<u32, &'static IrqPin>> = Mutex::new(BTreeMap::new());

// Configures a GSI and returns its pin, sharing pins between users of the same GSI.
pub fn system_irq(gsi: u32, trigger: IrqTrigger, polarity: IrqPolarity) -> Option<&'static IrqPin> {
    let mut pins = IRQ_PINS.lock().expect(EXPECT_LOCK);
    if let Some(pin) = pins.get(&gsi) {
        if pin.trigger != Some(trigger) || pin.polarity != Some(polarity) {
            println!("sif: Conflicting configurations for GSI {gsi}");
        }
        return Some(*pin);
    }

    let handle = match hel::access_irq_by_gsi(hardware_access_handle(), gsi as u64) {
        Ok(handle) => handle,
        Err(err) => {
            println!("sif: Failed to access GSI {gsi}: {err}");
            return None;
        }
    };
    if let Err(err) = hel::configure_irq(&handle, Some(trigger), Some(polarity)) {
        println!("sif: Failed to configure GSI {gsi}: {err}");
        return None;
    }

    let pin = leak(IrqPin {
        name: format!("gsi-{gsi}"),
        handle,
        trigger: Some(trigger),
        polarity: Some(polarity),
    });
    pins.insert(gsi, pin);
    Some(pin)
}

static DT_IRQ_PINS: Mutex<BTreeMap<(u32, u64), &'static IrqPin>> = Mutex::new(BTreeMap::new());

// Configures a DT interrupt and returns its pin, sharing pins between users of the same
// (controller, index) pair.
pub fn dt_irq(
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
