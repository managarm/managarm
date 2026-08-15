use managarm::svrctl::hardware_access_handle;
use std::collections::BTreeMap;
use std::sync::Mutex;

use super::{ConfigIoError, PciConfigIo, Result, check_offset};

const CONFIG_SPACE_SIZE: u16 = 0x1000;

pub struct EcamPcieConfigIo {
    mmio_base: u64,
    seg: u16,
    bus_start: u8,
    bus_end: u8,
    bus_mappings: Mutex<BTreeMap<u8, hel::Mapping<u8>>>,
}

impl EcamPcieConfigIo {
    pub fn new(mmio_base: u64, seg: u16, bus_start: u8, bus_end: u8) -> Self {
        Self {
            mmio_base,
            seg,
            bus_start,
            bus_end,
            bus_mappings: Mutex::new(BTreeMap::new()),
        }
    }

    fn check_address(&self, seg: u16, bus: u8, slot: u8, function: u8) -> Result<()> {
        if seg != self.seg
            || bus < self.bus_start
            || bus > self.bus_end
            || slot >= 32
            || function >= 8
        {
            return Err(ConfigIoError::InvalidAddress {
                seg,
                bus,
                slot,
                function,
            });
        }
        Ok(())
    }

    // Mappings are never removed, hence the returned pointer stays valid after we drop the lock.
    fn space_for_bus(&self, bus: u8) -> Result<*mut u8> {
        let mut mappings = self
            .bus_mappings
            .lock()
            .expect("sif: ECAM window mutex was poisoned");
        if let Some(mapping) = mappings.get(&bus) {
            return Ok(unsafe { mapping.as_ptr() }.unwrap().as_ptr());
        }

        const SIZE: usize = 1 << 20;
        let offset = ((bus - self.bus_start) as u64) << 20;

        let handle = hel::access_physical(
            hardware_access_handle(),
            (self.mmio_base + offset) as usize,
            SIZE,
            hel::CachingMode::Mmio,
        )
        .map_err(|source| ConfigIoError::MappingFailed {
            seg: self.seg,
            bus,
            source,
        })?;
        let mapping = unsafe {
            hel::Mapping::<u8>::new(
                &handle,
                None,
                0,
                SIZE,
                hel::MappingFlags::READ | hel::MappingFlags::WRITE,
            )
        }
        .map_err(|source| ConfigIoError::MappingFailed {
            seg: self.seg,
            bus,
            source,
        })?;

        let ptr = unsafe { mapping.as_ptr() }.unwrap().as_ptr();
        mappings.insert(bus, mapping);
        Ok(ptr)
    }

    fn calculate_offset(slot: u8, function: u8, offset: u16) -> usize {
        ((slot as usize) << 15) | ((function as usize) << 12) | offset as usize
    }
}

impl PciConfigIo for EcamPcieConfigIo {
    unsafe fn read_config_byte(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u8> {
        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 1, CONFIG_SPACE_SIZE)?;
        let space = self.space_for_bus(bus)?;
        let space_offset = Self::calculate_offset(slot, function, offset);
        Ok(unsafe { core::ptr::read_volatile(space.add(space_offset)) })
    }

    unsafe fn read_config_half(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u16> {
        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 2, CONFIG_SPACE_SIZE)?;
        let space = self.space_for_bus(bus)?;
        let space_offset = Self::calculate_offset(slot, function, offset);
        Ok(unsafe { core::ptr::read_volatile(space.add(space_offset) as *const u16) })
    }

    unsafe fn read_config_word(
        &self,
        seg: u16,
        bus: u8,
        slot: u8,
        function: u8,
        offset: u16,
    ) -> Result<u32> {
        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 4, CONFIG_SPACE_SIZE)?;
        let space = self.space_for_bus(bus)?;
        let space_offset = Self::calculate_offset(slot, function, offset);
        Ok(unsafe { core::ptr::read_volatile(space.add(space_offset) as *const u32) })
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
        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 1, CONFIG_SPACE_SIZE)?;
        let space = self.space_for_bus(bus)?;
        let space_offset = Self::calculate_offset(slot, function, offset);
        unsafe { core::ptr::write_volatile(space.add(space_offset), value) };
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
        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 2, CONFIG_SPACE_SIZE)?;
        let space = self.space_for_bus(bus)?;
        let space_offset = Self::calculate_offset(slot, function, offset);
        unsafe { core::ptr::write_volatile(space.add(space_offset) as *mut u16, value) };
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
        self.check_address(seg, bus, slot, function)?;
        check_offset(offset, 4, CONFIG_SPACE_SIZE)?;
        let space = self.space_for_bus(bus)?;
        let space_offset = Self::calculate_offset(slot, function, offset);
        unsafe { core::ptr::write_volatile(space.add(space_offset) as *mut u32, value) };
        Ok(())
    }

    fn supports_4k_config_space(&self) -> bool {
        true
    }
}
