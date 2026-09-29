use uacpi_sys::{acpi_gas, uacpi_address_space, uacpi_u64};

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

    pub fn read(&self) -> Result<u64> {
        let mut value: uacpi_u64 = 0;
        unsafe {
            check(
                "uacpi_gas_read",
                uacpi_sys::uacpi_gas_read(&self.0, &mut value),
            )?
        };
        Ok(value)
    }

    pub fn write(&self, value: u64) -> Result<()> {
        unsafe {
            check(
                "uacpi_gas_write",
                uacpi_sys::uacpi_gas_write(&self.0, value),
            )
        }
    }
}
