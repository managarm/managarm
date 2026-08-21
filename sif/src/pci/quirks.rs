use managarm::svrctl::hardware_access_handle;
use std::ptr::addr_of;

use super::PciDevice;

use crate::acpi::PAGE_MASK;

fn uhci_smi_disable(dev: &'static PciDevice) {
    println!("sif:     Disabling UHCI SMI generation!");
    let bus = dev.entity.parent_bus;
    // USBLEGSUP is defined by UHCI, not by PCI.
    unsafe { bus.write_config_half(dev.entity.slot, dev.entity.function, 0xC0, 0x2000) };
}

fn switch_usb_ports_to_xhci(dev: &'static PciDevice) {
    println!("sif:     Switching USB ports to XHCI!");
    let bus = dev.entity.parent_bus;

    // The port routing registers are specific to Intel's xHCI controllers.
    let usb3_ports_avail =
        unsafe { bus.read_config_word(dev.entity.slot, dev.entity.function, 0xDC) };
    unsafe { bus.write_config_word(dev.entity.slot, dev.entity.function, 0xD8, usb3_ports_avail) };

    let usb2_ports_avail =
        unsafe { bus.read_config_word(dev.entity.slot, dev.entity.function, 0xD4) };
    unsafe { bus.write_config_word(dev.entity.slot, dev.entity.function, 0xD0, usb2_ports_avail) };
}

const IGD_OPREGION_SIGNATURE: &[u8; 16] = b"IntelGraphicsMem";
const IGD_OPREGION_SIZE: u64 = 8 * 1024;

const IGD_OPREGION_ASLE_OFFSET: usize = 0x300;
const IGD_OPREGION_VBT_OFFSET: u64 = 0x400;
const IGD_OPREGION_ASLE_EXT_OFFSET: u64 = 0x1C00;

const IGD_MBOX_ASLE: u32 = 1 << 2;
const IGD_MBOX_VBT: u32 = 1 << 3;
const IGD_MBOX_ASLE_EXT: u32 = 1 << 4;

#[repr(C, packed)]
struct IgdOpregionHeader {
    signature: [u8; 16],
    size: u32,
    over_reserved: u8,
    over_revision: u8,
    over_minor: u8,
    over_major: u8,
    sver: [u8; 32],
    vver: [u8; 16],
    gver: [u8; 16],
    mbox: u32,
    dmod: u32,
    pcon: u32,
    dver: [u8; 32],
    reserved: [u8; 124],
}

const _: () = {
    assert!(size_of::<IgdOpregionHeader>() == 256);
};

#[repr(C, packed)]
struct IgdOpregionAsle {
    ardy: u32,
    aslc: u32,
    tche: u32,
    alsi: u32,
    bclp: u32,
    pfit: u32,
    cblv: u32,
    bclm: [u16; 20],
    cpfm: u32,
    epfm: u32,
    plut: [u8; 74],
    pfmb: u32,
    cddv: u32,
    pcft: u32,
    srot: u32,
    iuer: u32,
    fdss: u64,
    fdsp: u32,
    stat: u32,
    rvda: u64,
    rvds: u32,
    reserved: [u8; 58],
}

fn read_intel_integrated_graphics_vbt(dev: &'static PciDevice) {
    let bus = dev.entity.parent_bus;

    // ASLS is specific to Intel's integrated graphics.
    let asls_phys =
        (unsafe { bus.read_config_word(dev.entity.slot, dev.entity.function, 0xFC) }) as u64;
    if asls_phys == 0 {
        // ACPI OpRegion not supported.
        println!("sif:     ASLS unset, broken firmware? GPU unusable");
        return;
    }

    println!("sif:     OpRegion physical address {asls_phys:#x}");

    let page_off = (asls_phys as usize) & PAGE_MASK;
    let aligned = (asls_phys as usize) & !PAGE_MASK;
    let span = (0x2000 + page_off + PAGE_MASK) & !PAGE_MASK;

    let Ok(handle) = hel::access_physical(
        hardware_access_handle(),
        aligned,
        span,
        hel::CachingMode::Default,
    ) else {
        println!("sif:     Failed to access the OpRegion");
        return;
    };
    let Ok(mapping) =
        (unsafe { hel::Mapping::<u8>::new(&handle, None, 0, span, hel::MappingFlags::READ) })
    else {
        println!("sif:     Failed to map the OpRegion");
        return;
    };
    let base = unsafe { mapping.as_ptr() }.unwrap().as_ptr();
    let opregion = unsafe { base.add(page_off) } as *const IgdOpregionHeader;

    let signature = unsafe { addr_of!((*opregion).signature).read_unaligned() };
    if &signature != IGD_OPREGION_SIGNATURE {
        println!("sif:     OpRegion signature invalid, GPU unusable");
        return;
    }

    let major = unsafe { addr_of!((*opregion).over_major).read_unaligned() };
    let minor = unsafe { addr_of!((*opregion).over_minor).read_unaligned() };
    let revision = unsafe { addr_of!((*opregion).over_revision).read_unaligned() };

    println!("sif:     found ACPI OpRegion {major}.{minor}.{revision}");

    let mbox = unsafe { addr_of!((*opregion).mbox).read_unaligned() };

    let asle = if mbox & IGD_MBOX_ASLE != 0 {
        Some(
            unsafe { (opregion as *const u8).add(IGD_OPREGION_ASLE_OFFSET) }
                as *const IgdOpregionAsle,
        )
    } else {
        None
    };

    if major >= 2
        && let Some(asle) = asle
    {
        let mut rvda = unsafe { addr_of!((*asle).rvda).read_unaligned() };
        let rvds = unsafe { addr_of!((*asle).rvds).read_unaligned() };

        if rvda != 0 && rvds != 0 {
            // In OpRegion v2.1+, rvda was changed to a relative offset.
            if major > 2 || (major == 2 && minor >= 1) {
                if rvda < IGD_OPREGION_SIZE {
                    println!("sif:     VBT base shouldn't be within OpRegion, but it is!");
                }

                rvda += asls_phys;
            }

            // OpRegion 2.0: rvda is a physical address.
            dev.igd_vbt
                .set((rvda, rvds as u64))
                .expect("sif: Intel IGD VBT was already discovered");
            return;
        }
    }

    if mbox & IGD_MBOX_VBT == 0 {
        // The ACPI OpRegion does not support the VBT mailbox when it should.
        return;
    }

    let vbt_size = (if mbox & IGD_MBOX_ASLE_EXT != 0 {
        IGD_OPREGION_ASLE_EXT_OFFSET
    } else {
        IGD_OPREGION_SIZE
    }) - IGD_OPREGION_VBT_OFFSET;

    dev.igd_vbt
        .set((asls_phys + IGD_OPREGION_VBT_OFFSET, vbt_size))
        .expect("sif: Intel IGD VBT was already discovered");
}

fn enable_nvidia_hda(dev: &'static PciDevice) {
    if dev.entity.device_id < 0x08A0 {
        return;
    }

    let bus = dev.entity.parent_bus;
    // This register is specific to NVIDIA GPUs.
    let v = unsafe { bus.read_config_word(dev.entity.slot, 0, 0x488) };
    if v & (1 << 25) != 0 {
        return;
    }

    println!("sif:     Enabling HDA function on NVIDIA GPU");
    unsafe { bus.write_config_word(dev.entity.slot, 0, 0x488, v | (1 << 25)) };
}

struct Quirk {
    pci_class: Option<u8>,
    pci_subclass: Option<u8>,
    pci_interface: Option<u8>,
    pci_vendor: Option<u16>,
    pci_segment: Option<u16>,
    pci_bus: Option<u8>,
    pci_slot: Option<u8>,
    pci_func: Option<u8>,
    func: fn(&'static PciDevice),
}

const DEFAULT_QUIRK: Quirk = Quirk {
    pci_class: None,
    pci_subclass: None,
    pci_interface: None,
    pci_vendor: None,
    pci_segment: None,
    pci_bus: None,
    pci_slot: None,
    pci_func: None,
    func: |_| {},
};

// Like thor's quirk table, minus the RPi4 VL805 firmware upload (not ported yet).
static QUIRKS: &[Quirk] = &[
    Quirk {
        pci_class: Some(0x0C),
        pci_subclass: Some(0x03),
        pci_interface: Some(0x00),
        func: uhci_smi_disable,
        ..DEFAULT_QUIRK
    },
    Quirk {
        pci_class: Some(0x0C),
        pci_subclass: Some(0x03),
        pci_interface: Some(0x30),
        pci_vendor: Some(0x8086),
        func: switch_usb_ports_to_xhci,
        ..DEFAULT_QUIRK
    },
    Quirk {
        pci_class: Some(0x03),
        pci_subclass: Some(0x00),
        pci_vendor: Some(0x8086),
        pci_bus: Some(0),
        pci_slot: Some(2),
        pci_func: Some(0),
        func: read_intel_integrated_graphics_vbt,
        ..DEFAULT_QUIRK
    },
    Quirk {
        pci_class: Some(0x03),
        pci_vendor: Some(0x10de),
        func: enable_nvidia_hda,
        ..DEFAULT_QUIRK
    },
];

pub fn apply_pci_device_quirks(dev: &'static PciDevice) {
    for quirk in QUIRKS {
        if quirk.pci_class.is_some_and(|v| dev.entity.class_code != v) {
            continue;
        }

        if quirk
            .pci_subclass
            .is_some_and(|v| dev.entity.sub_class != v)
        {
            continue;
        }

        if quirk
            .pci_interface
            .is_some_and(|v| dev.entity.interface != v)
        {
            continue;
        }

        if quirk.pci_vendor.is_some_and(|v| dev.entity.vendor != v) {
            continue;
        }

        if quirk.pci_segment.is_some_and(|v| dev.entity.seg != v) {
            continue;
        }

        if quirk.pci_bus.is_some_and(|v| dev.entity.bus != v) {
            continue;
        }

        if quirk.pci_slot.is_some_and(|v| dev.entity.slot != v) {
            continue;
        }

        if quirk.pci_func.is_some_and(|v| dev.entity.function != v) {
            continue;
        }

        (quirk.func)(dev);
    }
}
