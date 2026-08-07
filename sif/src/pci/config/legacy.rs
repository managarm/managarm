use std::sync::Mutex;

use super::{ConfigIoError, PciConfigIo, Result, check_offset};
use crate::io;

const CONFIG_SPACE_SIZE: u16 = 0x100;

pub struct LegacyPciConfigIo {
    mutex: Mutex<()>,
}

impl LegacyPciConfigIo {
    pub const fn new() -> Self {
        Self {
            mutex: Mutex::new(()),
        }
    }

    fn check_address(seg: u16, bus: u8, slot: u8, function: u8) -> Result<()> {
        if seg != 0 || slot >= 32 || function >= 8 {
            return Err(ConfigIoError::InvalidAddress {
                seg,
                bus,
                slot,
                function,
            });
        }
        Ok(())
    }

    fn address(bus: u8, slot: u8, function: u8, offset: u16) -> u32 {
        0x8000_0000
            | ((bus as u32) << 16)
            | ((slot as u32) << 11)
            | ((function as u32) << 8)
            | (offset as u32 & !3)
    }
}

impl PciConfigIo for LegacyPciConfigIo {
    unsafe fn read_config_byte(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u8> {
        Self::check_address(seg, bus, slot, function)?;
        check_offset(offset, 1, CONFIG_SPACE_SIZE)?;
        let address = Self::address(bus, slot, function, offset);
        let _lock = self
            .mutex
            .lock()
            .expect("sif: legacy config space mutex was poisoned");
        Ok(unsafe {
            io::outl(0xCF8, address);
            io::inb(0xCFC + (offset & 3))
        })
    }

    unsafe fn read_config_half(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u16> {
        Self::check_address(seg, bus, slot, function)?;
        check_offset(offset, 2, CONFIG_SPACE_SIZE)?;
        let address = Self::address(bus, slot, function, offset);
        let _lock = self
            .mutex
            .lock()
            .expect("sif: legacy config space mutex was poisoned");
        Ok(unsafe {
            io::outl(0xCF8, address);
            io::inw(0xCFC + (offset & 3))
        })
    }

    unsafe fn read_config_word(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u32> {
        Self::check_address(seg, bus, slot, function)?;
        check_offset(offset, 4, CONFIG_SPACE_SIZE)?;
        let address = Self::address(bus, slot, function, offset);
        let _lock = self
            .mutex
            .lock()
            .expect("sif: legacy config space mutex was poisoned");
        Ok(unsafe {
            io::outl(0xCF8, address);
            io::inl(0xCFC)
        })
    }

    unsafe fn write_config_byte(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u8,
    ) -> Result<()> {
        Self::check_address(seg, bus, slot, function)?;
        check_offset(offset, 1, CONFIG_SPACE_SIZE)?;
        let address = Self::address(bus, slot, function, offset);
        let _lock = self
            .mutex
            .lock()
            .expect("sif: legacy config space mutex was poisoned");
        unsafe {
            io::outl(0xCF8, address);
            io::outb(0xCFC + (offset & 3), value);
        }
        Ok(())
    }

    unsafe fn write_config_half(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u16,
    ) -> Result<()> {
        Self::check_address(seg, bus, slot, function)?;
        check_offset(offset, 2, CONFIG_SPACE_SIZE)?;
        let address = Self::address(bus, slot, function, offset);
        let _lock = self
            .mutex
            .lock()
            .expect("sif: legacy config space mutex was poisoned");
        unsafe {
            io::outl(0xCF8, address);
            io::outw(0xCFC + (offset & 3), value);
        }
        Ok(())
    }

    unsafe fn write_config_word(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
        value: u32,
    ) -> Result<()> {
        Self::check_address(seg, bus, slot, function)?;
        check_offset(offset, 4, CONFIG_SPACE_SIZE)?;
        let address = Self::address(bus, slot, function, offset);
        let _lock = self
            .mutex
            .lock()
            .expect("sif: legacy config space mutex was poisoned");
        unsafe {
            io::outl(0xCF8, address);
            io::outl(0xCFC, value);
        }
        Ok(())
    }

    fn supports_4k_config_space(&self) -> bool {
        false
    }
}
