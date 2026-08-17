//! aarch64 interrupt controllers; port of the interrupt specifier decoding of thor's
//! thor-internal/arch/gic.hpp.

use crate::dt::fdt::Cells;
use crate::dt::node::DeviceTreeNode;

use super::{DtIrq, IrqController, decode_irq_flags};

static DT_GIC_V2_COMPATIBLE: [&str; 12] = [
    "arm,arm11mp-gic",
    "arm,cortex-a15-gic",
    "arm,cortex-a7-gic",
    "arm,cortex-a5-gic",
    "arm,cortex-a9-gic",
    "arm,eb11mp-gic",
    "arm,gic-400",
    "arm,pl390",
    "arm,tc11mp-gic",
    "nvidia,tegra210-agic",
    "qcom,msm-8660-qgic",
    "qcom,msm-qgic2",
];

static DT_GIC_V3_COMPATIBLE: [&str; 1] = ["arm,gic-v3"];

struct Gic;

impl IrqController for Gic {
    fn resolve_dt_irq(&self, irq_specifier: Cells<'static>) -> Option<DtIrq> {
        if irq_specifier.num_cells() != 3 && irq_specifier.num_cells() != 4 {
            panic!("GIC #interrupt-cells should be 3 or 4");
        }
        let type_ = irq_specifier
            .read_slice(0, 1)
            .expect("Failed to read GIC interrupt type");
        let idx = irq_specifier
            .read_slice(1, 1)
            .expect("Failed to read GIC interrupt index");
        let flags = irq_specifier
            .read_slice(2, 1)
            .expect("Failed to read GIC interrupt flags");

        // TODO(qookie): Handle extended PPI and SPI.
        if type_ != 0 && type_ != 1 {
            panic!("Unexpected GIC interrupt type {type_}");
        }

        // The upper flag bits carry the PPI CPU affinity, which we do not care about.
        let Some((trigger, _)) = decode_irq_flags(flags & 0xF) else {
            println!(
                "sif: Illegal IRQ flags {} found when parsing GIC interrupt",
                flags & 0xF
            );
            return None;
        };

        let irq = idx + if type_ == 1 { 16 } else { 32 };

        // The GIC does not support configuring IRQ polarity.
        Some(DtIrq {
            index: irq,
            trigger: Some(trigger),
            polarity: None,
        })
    }
}

pub fn lookup(node: &'static DeviceTreeNode) -> Option<&'static dyn IrqController> {
    if node.is_compatible(&DT_GIC_V2_COMPATIBLE) || node.is_compatible(&DT_GIC_V3_COMPATIBLE) {
        Some(&Gic)
    } else {
        None
    }
}
