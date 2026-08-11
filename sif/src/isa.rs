use hel::{IrqPolarity, IrqTrigger};
use uacpi_sys::acpi_madt_interrupt_source_override;
use zerocopy::FromBytes;

use crate::uacpi::table::Table;

/// Number of IRQs of the ISA bus.
const NUM_ISA_IRQS: usize = 16;

/// ISA IRQs that we configure upfront, i.e., before any driver claims them.
///
/// TODO: This is a hack. We assume that HPET will use legacy replacement.
const PRECONFIGURED_ISA_IRQS: [u8; 5] = [0, 1, 4, 12, 14];

/// The GSI that an ISA IRQ is wired to, and how the interrupt controller drives it.
#[derive(Clone, Copy)]
struct IsaIrq {
    gsi: u32,
    trigger: IrqTrigger,
    polarity: IrqPolarity,
}

impl IsaIrq {
    /// Without an override, ISA IRQs are identity mapped to GSIs and use the ISA bus defaults.
    fn identity(irq: u8) -> IsaIrq {
        IsaIrq {
            gsi: irq.into(),
            trigger: IrqTrigger::Edge,
            polarity: IrqPolarity::High,
        }
    }
}

/// Interrupt source overrides of the MADT, indexed by ISA IRQ.
type IsaOverrides = [Option<IsaIrq>; NUM_ISA_IRQS];

/// Collects the interrupt source overrides of the MADT.
fn read_isa_overrides() -> IsaOverrides {
    let mut overrides: IsaOverrides = [None; NUM_ISA_IRQS];

    let madt = match Table::find_by_signature(c"APIC") {
        Ok(Some(madt)) => madt,
        Ok(None) => {
            println!("sif: No MADT table, assuming that no ISA IRQs are overridden");
            return overrides;
        }
        Err(err) => {
            println!("sif: Failed to find the MADT: {err}");
            return overrides;
        }
    };

    for (entry_type, subtable) in madt.subtables(size_of::<uacpi_sys::acpi_madt>()) {
        if u32::from(entry_type) != uacpi_sys::ACPI_MADT_ENTRY_TYPE_INTERRUPT_SOURCE_OVERRIDE {
            continue;
        }
        let Ok((entry, _)) = acpi_madt_interrupt_source_override::read_from_prefix(subtable) else {
            println!("sif: Ignoring truncated MADT interrupt source override");
            continue;
        };
        let bus = entry.bus;
        let source = entry.source;
        let gsi = entry.gsi;
        let flags = u32::from(entry.flags);

        // ACPI only defines overrides for the ISA bus.
        if bus != 0 || usize::from(source) >= NUM_ISA_IRQS {
            println!("sif: Ignoring MADT override of bus {bus} IRQ {source}");
            continue;
        }
        if overrides[usize::from(source)].is_some() {
            println!("sif: Ignoring duplicate MADT override of ISA IRQ {source}");
            continue;
        }

        // Interrupts that conform to the bus use the ISA defaults.
        let trigger = match flags & uacpi_sys::ACPI_MADT_TRIGGERING_MASK {
            uacpi_sys::ACPI_MADT_TRIGGERING_LEVEL => IrqTrigger::Level,
            _ => IrqTrigger::Edge,
        };
        let polarity = match flags & uacpi_sys::ACPI_MADT_POLARITY_MASK {
            uacpi_sys::ACPI_MADT_POLARITY_ACTIVE_LOW => IrqPolarity::Low,
            _ => IrqPolarity::High,
        };

        overrides[usize::from(source)] = Some(IsaIrq {
            gsi,
            trigger,
            polarity,
        });
    }

    overrides
}

/// Configures the ISA IRQs of devices whose drivers cannot resolve them themselves.
pub fn configure_isa_irqs() {
    let overrides = read_isa_overrides();

    println!("sif: Configuring ISA IRQs");
    for irq in PRECONFIGURED_ISA_IRQS {
        let line = overrides[usize::from(irq)].unwrap_or_else(|| IsaIrq::identity(irq));
        let pin = match hel::access_irq(line.gsi as i32) {
            Ok(pin) => pin,
            Err(err) => {
                println!(
                    "sif: Failed to access ISA IRQ {irq} (GSI {}): {err}",
                    line.gsi
                );
                continue;
            }
        };
        if let Err(err) = hel::configure_irq(&pin, line.trigger, line.polarity) {
            println!(
                "sif: Failed to configure ISA IRQ {irq} (GSI {}): {err}",
                line.gsi
            );
        }
    }
}
