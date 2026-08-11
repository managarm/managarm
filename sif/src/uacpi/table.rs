use std::ffi::CStr;
use std::slice;

use uacpi_sys::{acpi_entry_hdr, acpi_sdt_hdr};
use zerocopy::FromBytes;

use super::{Error, Result};

/// A reference to an ACPI table, i.e., a table that uACPI keeps mapped for us.
pub struct Table {
    table: uacpi_sys::uacpi_table,
}

impl Table {
    /// Wraps uacpi_table_find_by_signature().
    pub fn find_by_signature(signature: &CStr) -> Result<Option<Table>> {
        let mut table = uacpi_sys::uacpi_table::default();
        // SAFETY: uACPI only reads the signature and only writes to the table.
        let status =
            unsafe { uacpi_sys::uacpi_table_find_by_signature(signature.as_ptr(), &mut table) };
        match status {
            uacpi_sys::UACPI_STATUS_OK => (),
            uacpi_sys::UACPI_STATUS_NOT_FOUND => return Ok(None),
            _ => return Err(Error::new("uacpi_table_find_by_signature", status)),
        }

        let table = Table { table };
        // uACPI does not hand out tables that it failed to map.
        if table.hdr().is_null() {
            return Ok(None);
        }
        Ok(Some(table))
    }

    fn hdr(&self) -> *const acpi_sdt_hdr {
        // SAFETY: the union only holds different representations of the same pointer.
        unsafe { self.table.__bindgen_anon_1.hdr }
    }

    /// Returns the bytes of the table, header included.
    fn bytes(&self) -> &[u8] {
        // SAFETY: uACPI maps whole tables, hence at least the header is accessible.
        let header =
            unsafe { slice::from_raw_parts(self.hdr().cast::<u8>(), size_of::<acpi_sdt_hdr>()) };
        let (hdr, _) = acpi_sdt_hdr::read_from_prefix(header)
            .expect("sif: slice of an ACPI table header is too small");

        // SAFETY: the table stays mapped for as long as we hold our reference to it.
        unsafe { slice::from_raw_parts(self.hdr().cast::<u8>(), hdr.length as usize) }
    }

    /// Iterates the subtables that follow the first `header_size` bytes of the table,
    /// like uacpi_for_each_subtable() does.
    pub fn subtables(&self, header_size: usize) -> Subtables<'_> {
        Subtables {
            rest: self.bytes().get(header_size..).unwrap_or_default(),
        }
    }
}

impl Drop for Table {
    /// Wraps uacpi_table_unref().
    fn drop(&mut self) {
        // SAFETY: we drop the only reference that we took in find_by_signature().
        unsafe { uacpi_sys::uacpi_table_unref(&mut self.table) };
    }
}

/// Iterator over the subtables of an ACPI table such as the MADT.
pub struct Subtables<'a> {
    rest: &'a [u8],
}

impl<'a> Iterator for Subtables<'a> {
    /// The type of the subtable and its bytes, header included.
    type Item = (u8, &'a [u8]);

    fn next(&mut self) -> Option<(u8, &'a [u8])> {
        let (hdr, _) = acpi_entry_hdr::read_from_prefix(self.rest).ok()?;

        // Give up instead of spinning forever on a table that we cannot make sense of.
        let length = usize::from(hdr.length);
        if length < size_of::<acpi_entry_hdr>() {
            println!("sif: Ignoring subtable with a length of {length} bytes");
            return None;
        }

        let subtable = self.rest.get(..length)?;
        self.rest = &self.rest[length..];
        Some((hdr.type_, subtable))
    }
}
