//! Parser for the DMA Remapping (DMAR) table, which describes the Intel VT-d units.

use zerocopy::{FromBytes, Immutable, KnownLayout};

use crate::uacpi::table::Table;

/// Size of the DMAR header, i.e., the offset of the first remapping structure.
const HEADER_SIZE: usize = 48;

const REMAPPING_TYPE_DRHD: u16 = 0;
const REMAPPING_TYPE_RMRR: u16 = 1;

const DRHD_FLAGS_INCLUDE_PCI_ALL: u8 = 1;

/// Device scope types (VT-d specification rev 4.1, 8.3.1).
pub const SCOPE_PCI_ENDPOINT: u8 = 1;
pub const SCOPE_PCI_BRIDGE: u8 = 2;

#[derive(FromBytes, Immutable, KnownLayout)]
#[repr(C, packed)]
struct RawRemappingHeader {
    type_: u16,
    length: u16,
}

#[derive(FromBytes, Immutable, KnownLayout)]
#[repr(C, packed)]
struct RawDrhd {
    hdr: RawRemappingHeader,
    flags: u8,
    size: u8,
    segment: u16,
    register_base: u64,
}

#[derive(FromBytes, Immutable, KnownLayout)]
#[repr(C, packed)]
struct RawRmrr {
    hdr: RawRemappingHeader,
    reserved: u16,
    segment: u16,
    base: u64,
    limit: u64,
}

#[derive(FromBytes, Immutable, KnownLayout)]
#[repr(C, packed)]
struct RawDeviceScope {
    type_: u8,
    length: u8,
    reserved: u16,
    enumeration_id: u8,
    start_bus: u8,
}

/// Names a device by the path that firmware enumerated it through.
pub struct DeviceScope {
    pub type_: u8,
    pub start_bus: u8,
    /// (slot, function) pairs, from the start bus down to the device itself.
    pub path: Vec<(u8, u8)>,
}

/// One DMA Remapping Hardware Unit Definition, i.e., one IOMMU.
pub struct Drhd {
    pub segment: u16,
    pub register_base: u64,
    /// Whether the unit covers every device of its segment that no other unit claims.
    pub include_pci_all: bool,
    pub scopes: Vec<DeviceScope>,
}

/// One Reserved Memory Region Reporting structure.
pub struct Rmrr {
    pub segment: u16,
    pub base: u64,
    pub limit: u64,
    pub scopes: Vec<DeviceScope>,
}

pub struct Dmar {
    pub drhds: Vec<Drhd>,
    pub rmrrs: Vec<Rmrr>,
}

fn parse_scopes(mut rest: &[u8]) -> Vec<DeviceScope> {
    let mut scopes = Vec::new();

    while let Ok((hdr, _)) = RawDeviceScope::read_from_prefix(rest) {
        // Give up instead of spinning forever on a scope that we cannot make sense of.
        let length = usize::from(hdr.length);
        if length < size_of::<RawDeviceScope>() || length % 2 != 0 || length > rest.len() {
            println!("sif: Ignoring DMAR device scope with a length of {length} bytes");
            break;
        }

        scopes.push(DeviceScope {
            type_: hdr.type_,
            start_bus: hdr.start_bus,
            path: rest[size_of::<RawDeviceScope>()..length]
                .chunks_exact(2)
                .map(|entry| (entry[0], entry[1]))
                .collect(),
        });

        rest = &rest[length..];
    }

    scopes
}

/// Parses the DMAR table. Returns `None` if firmware does not provide one.
pub fn parse() -> Option<Dmar> {
    let table = match Table::find_by_signature(c"DMAR") {
        Ok(Some(table)) => table,
        Ok(None) => return None,
        Err(err) => {
            println!("sif: Failed to look up the DMAR table: {err}");
            return None;
        }
    };

    let mut dmar = Dmar {
        drhds: Vec::new(),
        rmrrs: Vec::new(),
    };
    let mut rest = table.bytes().get(HEADER_SIZE..)?;

    while let Ok((hdr, _)) = RawRemappingHeader::read_from_prefix(rest) {
        // Give up instead of spinning forever on a table that we cannot make sense of.
        let length = usize::from(hdr.length);
        if length < size_of::<RawRemappingHeader>() || length > rest.len() {
            println!("sif: Ignoring DMAR remapping structure with a length of {length} bytes");
            break;
        }
        let structure = &rest[..length];

        match hdr.type_ {
            REMAPPING_TYPE_DRHD => {
                if let Ok((raw, _)) = RawDrhd::read_from_prefix(structure) {
                    dmar.drhds.push(Drhd {
                        segment: raw.segment,
                        register_base: raw.register_base,
                        include_pci_all: raw.flags & DRHD_FLAGS_INCLUDE_PCI_ALL != 0,
                        scopes: parse_scopes(&structure[size_of::<RawDrhd>()..]),
                    });
                }
            }
            REMAPPING_TYPE_RMRR => {
                if let Ok((raw, _)) = RawRmrr::read_from_prefix(structure) {
                    dmar.rmrrs.push(Rmrr {
                        segment: raw.segment,
                        base: raw.base,
                        limit: raw.limit,
                        scopes: parse_scopes(&structure[size_of::<RawRmrr>()..]),
                    });
                }
            }
            _ => println!("sif: Ignoring DMAR remapping structure of type {}", {
                hdr.type_
            }),
        }

        rest = &rest[length..];
    }

    Some(dmar)
}
