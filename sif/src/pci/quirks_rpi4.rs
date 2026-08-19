use arch::{BitValue, IoMemSpace, bit_register, cache_invalidate, cache_writeback};
use managarm::posix;
use managarm::svrctl::hardware_access_handle;
use std::ffi::c_void;
use thiserror::Error;

use super::PciDevice;

use crate::acpi::{PAGE_MASK, PAGE_SIZE};
use crate::dt::node::get_device_tree_root;

bit_register! {
    MboxRead @ 0x00: u32 {
        CHANNEL @ 0, 4: u8;
        VALUE @ 4, 28: u32;
    }
}

bit_register! {
    MboxStatus @ 0x18: u32 {
        EMPTY @ 30, 1: bool;
        FULL @ 31, 1: bool;
    }
}

bit_register! {
    MboxWrite @ 0x20: u32 {
        CHANNEL @ 0, 4: u8;
        VALUE @ 4, 28: u32;
    }
}

const RPI_FIRMWARE_STATUS_REQUEST: u32 = 0;
const RPI_FIRMWARE_STATUS_SUCCESS: u32 = 0x80000000;

const RPI_FIRMWARE_NOTIFY_XHCI_RESET: u32 = 0x00030058;

#[derive(Debug, Error)]
enum MboxError {
    #[error("the firmware returned buffer {returned:#x} instead of {expected:#x}")]
    BufferMismatch { expected: usize, returned: usize },

    #[error("the firmware failed the request with status {status:#x}")]
    RequestFailed { status: u32 },
}

struct Bcm2835Mbox {
    space: IoMemSpace,
    // Physical address of the property buffer and the mapping that backs it.
    buf: usize,
    buf_mapping: hel::Mapping<u8>,
}

impl Bcm2835Mbox {
    fn new(base: u64) -> Result<Bcm2835Mbox, hel::Error> {
        let offset = base as usize & PAGE_MASK;

        let handle = hel::access_physical(
            hardware_access_handle(),
            base as usize & !PAGE_MASK,
            PAGE_SIZE,
            hel::CachingMode::Mmio,
        )?;
        let mapping = unsafe {
            hel::Mapping::<u8>::new(
                &handle,
                None,
                0,
                PAGE_SIZE,
                hel::MappingFlags::READ | hel::MappingFlags::WRITE,
            )
        }?;
        // The mapping is never removed, hence the pointer stays valid after we leak it.
        let mapping = Box::leak(Box::new(mapping));

        // The firmware accesses the property buffer through the 32-bit VideoCore bus.
        let buf_handle = hel::allocate_memory(
            posix::hierarchy_handle(),
            PAGE_SIZE,
            hel::AllocFlags::CONTINUOUS,
            Some(32),
        )?;
        let buf_mapping = unsafe {
            hel::Mapping::<u8>::new(
                &buf_handle,
                None,
                0,
                PAGE_SIZE,
                hel::MappingFlags::READ | hel::MappingFlags::WRITE,
            )
        }?;
        let buf = hel::pointer_physical(
            unsafe { buf_mapping.as_ptr() }.unwrap().as_ptr() as *const c_void
        )?;

        Ok(Bcm2835Mbox {
            space: unsafe { IoMemSpace::new(mapping.as_ptr().unwrap().as_ptr(), PAGE_SIZE) }
                .subspace(offset),
            buf,
            buf_mapping,
        })
    }

    fn write(&self, value: u32) {
        while unsafe { self.space.load(MboxStatus) }.get(MboxStatus::FULL) {}

        unsafe {
            self.space.store(
                MboxWrite,
                BitValue::zero()
                    .with(MboxWrite::CHANNEL, 8)
                    .with(MboxWrite::VALUE, value >> 4),
            )
        };
    }

    fn read(&self) -> u32 {
        while unsafe { self.space.load(MboxStatus) }.get(MboxStatus::EMPTY) {}

        let f = unsafe { self.space.load(MboxRead) };

        f.get(MboxRead::VALUE) << 4
    }

    fn send_property_list(&self, data: &[u32]) -> Result<(), MboxError> {
        let data_byte_size = std::mem::size_of_val(data);

        let header = [data_byte_size as u32 + 12, RPI_FIRMWARE_STATUS_REQUEST];

        assert!(data_byte_size + 12 <= PAGE_SIZE);

        let buf = unsafe { self.buf_mapping.as_ptr() }.unwrap().as_ptr();
        unsafe {
            buf.write_bytes(0, PAGE_SIZE);
            buf.copy_from_nonoverlapping(header.as_ptr() as *const u8, size_of_val(&header));
            buf.add(size_of_val(&header))
                .copy_from_nonoverlapping(data.as_ptr() as *const u8, data_byte_size);
        }
        // List of properties is terminated by a 0 word set by the memset.
        cache_writeback(buf as usize, PAGE_SIZE);

        self.write(self.buf as u32);
        let out = self.read();

        if out as usize != self.buf {
            return Err(MboxError::BufferMismatch {
                expected: self.buf,
                returned: out as usize,
            });
        }

        cache_invalidate(buf as usize, PAGE_SIZE);
        let result = unsafe { (buf as *const u32).add(1).read_unaligned() };

        if result != RPI_FIRMWARE_STATUS_SUCCESS {
            return Err(MboxError::RequestFailed { status: result });
        }
        Ok(())
    }
}

pub fn upload_raspberry_pi4_vl805_firmware(dev: &'static PciDevice) {
    // Not a VL805.
    if dev.entity.device_id != 0x3483 {
        return;
    }

    let Some(root) = get_device_tree_root() else {
        return;
    };

    let mut rpi_fw_reset_node = None;
    root.for_each(&mut |node| {
        if node.is_compatible(&["raspberrypi,firmware-reset"]) {
            rpi_fw_reset_node = Some(node);
            return true;
        }

        false
    });

    // Not on a RPi4.
    if rpi_fw_reset_node.is_none() {
        return;
    }

    let mut rpi_mbox_node = None;
    root.for_each(&mut |node| {
        if node.is_compatible(&["brcm,bcm2835-mbox"]) {
            rpi_mbox_node = Some(node);
            return true;
        }

        false
    });

    // Definitely not on a RPi4.
    let Some(rpi_mbox_node) = rpi_mbox_node else {
        return;
    };
    let [mbox_reg] = rpi_mbox_node.reg() else {
        println!(
            "sif:     BCM2835 mailbox has {} reg entries instead of one",
            rpi_mbox_node.reg().len()
        );
        return;
    };

    let mbox = match Bcm2835Mbox::new(mbox_reg.addr) {
        Ok(mbox) => mbox,
        Err(err) => {
            println!("sif:     Failed to set up the BCM2835 mailbox: {err}");
            return;
        }
    };

    println!("sif:     Uploading VL805 firmware via Raspberry Pi4 firmware interface.");

    let addr = ((dev.entity.bus as u32) << 20)
        | ((dev.entity.slot as u32) << 15)
        | ((dev.entity.function as u32) << 12);

    let data = [
        RPI_FIRMWARE_NOTIFY_XHCI_RESET,
        size_of::<u32>() as u32, // request data size
        0,                       // response data size
        addr,
    ];

    if let Err(err) = mbox.send_property_list(&data) {
        println!("sif:     Failed to upload the VL805 firmware: {err}");
    }
}
