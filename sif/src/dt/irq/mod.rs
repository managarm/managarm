//! Device tree interrupt controllers; port of thor's thor-internal/dtb/irq.hpp.

mod aarch64;
mod riscv64;

use hel::{IrqPolarity, IrqTrigger};

use crate::dt::fdt::Cells;
use crate::dt::node::get_device_tree_root;

/// A decoded DT interrupt specifier: a controller-specific IRQ index (as accepted by
/// helAccessIrq) plus the trigger mode and polarity that the specifier requests.
///
/// The trigger mode and polarity are None if the interrupt controller cannot configure them.
pub struct DtIrq {
    pub index: u64,
    pub trigger: Option<IrqTrigger>,
    pub polarity: Option<IrqPolarity>,
}

pub trait IrqController: Sync {
    // Resolve a DT interrupt specifier to a controller-specific IRQ index.
    fn resolve_dt_irq(&self, irq_specifier: Cells<'static>) -> Option<DtIrq>;
}

// Decodes the IRQ_TYPE_* flags that the GIC and the APLIC share.
fn decode_irq_flags(flags: u64) -> Option<(IrqTrigger, IrqPolarity)> {
    match flags {
        1 => Some((IrqTrigger::Edge, IrqPolarity::High)),
        2 => Some((IrqTrigger::Edge, IrqPolarity::Low)),
        4 => Some((IrqTrigger::Level, IrqPolarity::High)),
        8 => Some((IrqTrigger::Level, IrqPolarity::Low)),
        _ => None,
    }
}

/// Associates each DT node that stands for a supported IRQ controller with that controller.
pub fn init() {
    let Some(root) = get_device_tree_root() else {
        return;
    };

    root.for_each(&mut |node| {
        if let Some(controller) = aarch64::lookup(node).or_else(|| riscv64::lookup(node)) {
            println!("sif: Found IRQ controller \"{}\"", node.path());
            node.associate_irq_controller(controller);
        }

        false
    });
}
