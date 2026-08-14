use std::ptr::NonNull;

use uacpi_sys::uacpi_pci_routing_table;

use super::namespace::NamespaceNode;
use super::{Result, check_optional};

/// An entry of a PCI routing table, i.e., of a _PRT.
pub struct RoutingEntry {
    pub address: u32,
    pub index: u32,
    pub source: Option<NamespaceNode>,
    pub pin: u8,
}

/// A PCI routing table that uACPI allocated for us.
pub struct RoutingTable {
    table: NonNull<uacpi_pci_routing_table>,
}

impl NamespaceNode {
    /// Wraps uacpi_get_pci_routing_table(), i.e., evaluates _PRT.
    /// Returns None if the device has no _PRT.
    pub fn pci_routing_table(self) -> Result<Option<RoutingTable>> {
        let mut table: *mut uacpi_pci_routing_table = std::ptr::null_mut();
        // SAFETY: uACPI only writes the pointer to the table that it allocates.
        let status = unsafe { uacpi_sys::uacpi_get_pci_routing_table(self.as_raw(), &mut table) };
        if !check_optional("uacpi_get_pci_routing_table", status)? {
            return Ok(None);
        }

        // uACPI does not hand out tables that it failed to allocate.
        let table = NonNull::new(table).expect("uACPI returned no PCI routing table");
        Ok(Some(RoutingTable { table }))
    }
}

impl RoutingTable {
    /// Iterates the entries of the table.
    pub fn entries(&self) -> impl Iterator<Item = RoutingEntry> + '_ {
        // SAFETY: we hold the only reference to the table, which is followed by num_entries
        // entries.
        let entries = unsafe {
            let table = self.table.as_ref();
            table.entries.as_slice(table.num_entries)
        };

        entries.iter().map(|entry| RoutingEntry {
            address: entry.address,
            index: entry.index,
            source: NamespaceNode::from_raw(entry.source),
            pin: entry.pin,
        })
    }
}

impl Drop for RoutingTable {
    /// Wraps uacpi_free_pci_routing_table().
    fn drop(&mut self) {
        // SAFETY: we free the table that we obtained in pci_routing_table().
        unsafe { uacpi_sys::uacpi_free_pci_routing_table(self.table.as_ptr()) };
    }
}
