//! riscv64 interrupt controllers; port of the interrupt specifier decoding of thor's
//! arch/riscv/plic.cpp and arch/riscv/aplic.cpp.

use crate::dt::DtError;
use crate::dt::fdt::Cells;
use crate::dt::node::DeviceTreeNode;

use super::{DtIrq, IrqController, decode_irq_flags};

static DT_PLIC_COMPATIBLE: [&str; 1] = ["riscv,plic0"];

static DT_APLIC_COMPATIBLE: [&str; 1] = ["riscv,aplic"];

struct Plic;

impl IrqController for Plic {
    fn resolve_dt_irq(&self, irq_specifier: Cells<'static>) -> Result<DtIrq, DtError> {
        let cells = irq_specifier.num_cells();
        if cells != 1 {
            return Err(DtError::UnsupportedInterruptCells { cells });
        }
        let idx = irq_specifier
            .read()
            .expect("Failed to read PLIC interrupt specifier");

        // The PLIC does not care about trigger mode / polarity.
        Ok(DtIrq {
            index: idx,
            trigger: None,
            polarity: None,
        })
    }
}

struct Aplic;

impl IrqController for Aplic {
    fn resolve_dt_irq(&self, irq_specifier: Cells<'static>) -> Result<DtIrq, DtError> {
        let cells = irq_specifier.num_cells();
        if cells != 2 {
            return Err(DtError::UnsupportedInterruptCells { cells });
        }
        let idx = irq_specifier
            .read_slice(0, 1)
            .expect("Failed to read APLIC interrupt index");
        let flags = irq_specifier
            .read_slice(1, 1)
            .expect("Failed to read APLIC interrupt flags");

        let (trigger, polarity) =
            decode_irq_flags(flags).ok_or(DtError::InvalidIrqFlags { flags })?;

        Ok(DtIrq {
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
