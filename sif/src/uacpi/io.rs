use std::ptr::NonNull;

use uacpi_sys::{acpi_gas, uacpi_address_space, uacpi_mapped_gas, uacpi_u64};

use super::{Result, check};

#[derive(Clone, Copy)]
pub struct Gas(acpi_gas);

impl Gas {
    pub fn new(space: uacpi_address_space, address: u64, bit_width: u8) -> Gas {
        Gas(acpi_gas {
            address_space_id: space as u8,
            register_bit_width: bit_width,
            register_bit_offset: 0,
            access_size: 0,
            address,
        })
    }

    pub fn from_raw(gas: acpi_gas) -> Gas {
        Gas(gas)
    }

    /// Wraps uacpi_map_gas().
    pub fn map(&self) -> Result<MappedGas> {
        let mut mapped: *mut uacpi_mapped_gas = std::ptr::null_mut();
        // SAFETY: uACPI only reads the GAS and only writes the pointer to the mapping that it
        // allocates.
        check("uacpi_map_gas", unsafe {
            uacpi_sys::uacpi_map_gas(&self.0, &mut mapped)
        })?;
        Ok(MappedGas(
            NonNull::new(mapped).expect("sif: uacpi_map_gas() returned a null mapping"),
        ))
    }
}

/// A register that is mapped once, unlike uacpi_gas_read() and uacpi_gas_write() which map
/// it on every access.
pub struct MappedGas(NonNull<uacpi_mapped_gas>);

// SAFETY: uACPI never changes a mapping after uacpi_map_gas(), and accesses go through
// uacpi_kernel_*() functions that may be called from any thread.
unsafe impl Send for MappedGas {}
unsafe impl Sync for MappedGas {}

impl MappedGas {
    /// Wraps uacpi_gas_read_mapped().
    pub fn read(&self) -> Result<u64> {
        let mut value: uacpi_u64 = 0;
        // SAFETY: the mapping is alive, and uACPI only writes to value.
        check("uacpi_gas_read_mapped", unsafe {
            uacpi_sys::uacpi_gas_read_mapped(self.0.as_ptr(), &mut value)
        })?;
        Ok(value)
    }

    /// Wraps uacpi_gas_write_mapped().
    pub fn write(&self, value: u64) -> Result<()> {
        // SAFETY: the mapping is alive.
        check("uacpi_gas_write_mapped", unsafe {
            uacpi_sys::uacpi_gas_write_mapped(self.0.as_ptr(), value)
        })
    }
}

impl Drop for MappedGas {
    fn drop(&mut self) {
        // SAFETY: the mapping came from uacpi_map_gas() and nothing uses it anymore.
        unsafe { uacpi_sys::uacpi_unmap_gas(self.0.as_ptr()) };
    }
}
