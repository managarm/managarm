use arch::{PioSpace, scalar_register};
use std::sync::Mutex;

use super::{ConfigIoError, PciConfigIo, Result, check_offset};

const CONFIG_SPACE_SIZE: u16 = 0x100;

scalar_register!(ConfigAddress @ 0x00: u32);
const DATA: usize = 4;

pub struct LegacyPciConfigIo {
    space: PioSpace,
    mutex: Mutex<()>,
}

impl LegacyPciConfigIo {
    pub const fn new() -> Self {
        Self {
            // The ports of the config window are enabled before we are constructed.
            space: unsafe { PioSpace::new(0xCF8) },
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
            self.space.store(ConfigAddress, address);
            self.space.scalar_load::<u8>(DATA + (offset & 3) as usize)
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
            self.space.store(ConfigAddress, address);
            self.space.scalar_load::<u16>(DATA + (offset & 3) as usize)
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
            self.space.store(ConfigAddress, address);
            self.space.scalar_load::<u32>(DATA)
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
            self.space.store(ConfigAddress, address);
            self.space
                .scalar_store::<u8>(DATA + (offset & 3) as usize, value);
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
            self.space.store(ConfigAddress, address);
            self.space
                .scalar_store::<u16>(DATA + (offset & 3) as usize, value);
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
            self.space.store(ConfigAddress, address);
            self.space.scalar_store::<u32>(DATA, value);
        }
        Ok(())
    }

    fn supports_4k_config_space(&self) -> bool {
        false
    }
}
