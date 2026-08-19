//! riscv64 interrupt controllers; port of the interrupt specifier decoding of thor's
//! arch/riscv/plic.cpp and arch/riscv/aplic.cpp.

use crate::dt::fdt::Cells;
use crate::dt::node::DeviceTreeNode;

use super::{DtIrq, IrqController, decode_irq_flags};

static DT_PLIC_COMPATIBLE: [&str; 1] = ["riscv,plic0"];

static DT_APLIC_COMPATIBLE: [&str; 1] = ["riscv,aplic"];

struct Plic;

impl IrqController for Plic {
    fn resolve_dt_irq(&self, irq_specifier: Cells<'static>) -> Option<DtIrq> {
        if irq_specifier.num_cells() != 1 {
            panic!("PLIC #interrupt-cells should be 1");
        }
        let idx = irq_specifier
            .read()
            .expect("Failed to read PLIC interrupt specifier");

        // The PLIC does not care about trigger mode / polarity.
        Some(DtIrq {
            index: idx,
            trigger: None,
            polarity: None,
        })
    }
}

struct Aplic;

impl IrqController for Aplic {
    fn resolve_dt_irq(&self, irq_specifier: Cells<'static>) -> Option<DtIrq> {
        if irq_specifier.num_cells() != 2 {
            panic!("APLIC #interrupt-cells should be 2");
        }
        let idx = irq_specifier
            .read_slice(0, 1)
            .expect("Failed to read APLIC interrupt index");
        let flags = irq_specifier
            .read_slice(1, 1)
            .expect("Failed to read APLIC interrupt flags");

        let Some((trigger, polarity)) = decode_irq_flags(flags) else {
            println!("sif: Illegal IRQ flags {flags} found when parsing APLIC interrupt");
            return None;
        };

        Some(DtIrq {
            index: idx,
            trigger: Some(trigger),
            polarity: Some(polarity),
        })
    }
}

pub fn lookup(node: &'static DeviceTreeNode) -> Option<&'static dyn IrqController> {
    if node.is_compatible(&DT_PLIC_COMPATIBLE) {
        Some(&Plic)
    } else if node.is_compatible(&DT_APLIC_COMPATIBLE) {
        Some(&Aplic)
    } else {
        None
    }
}
