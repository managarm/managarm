//! Standard USB descriptors and a walker over configuration descriptor buffers.

pub const DESCRIPTOR_TYPE_DEVICE: u8 = 0x01;
pub const DESCRIPTOR_TYPE_CONFIGURATION: u8 = 0x02;
pub const DESCRIPTOR_TYPE_STRING: u8 = 0x03;
pub const DESCRIPTOR_TYPE_INTERFACE: u8 = 0x04;
pub const DESCRIPTOR_TYPE_ENDPOINT: u8 = 0x05;

fn u16_at(bytes: &[u8], offset: usize) -> u16 {
    u16::from_le_bytes([bytes[offset], bytes[offset + 1]])
}

#[derive(Debug, Clone, Copy, Default)]
pub struct DeviceDescriptor {
    pub bcd_usb: u16,
    pub device_class: u8,
    pub device_subclass: u8,
    pub device_protocol: u8,
    pub max_packet_size0: u8,
    pub id_vendor: u16,
    pub id_product: u16,
    pub bcd_device: u16,
    pub manufacturer: u8,
    pub product: u8,
    pub serial_number: u8,
    pub num_configurations: u8,
}

impl DeviceDescriptor {
    pub const SIZE: usize = 18;

    pub fn parse(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < Self::SIZE {
            return None;
        }
        Some(Self {
            bcd_usb: u16_at(bytes, 2),
            device_class: bytes[4],
            device_subclass: bytes[5],
            device_protocol: bytes[6],
            max_packet_size0: bytes[7],
            id_vendor: u16_at(bytes, 8),
            id_product: u16_at(bytes, 10),
            bcd_device: u16_at(bytes, 12),
            manufacturer: bytes[14],
            product: bytes[15],
            serial_number: bytes[16],
            num_configurations: bytes[17],
        })
    }
}

#[derive(Debug, Clone, Copy, Default)]
pub struct ConfigDescriptor {
    pub total_length: u16,
    pub num_interfaces: u8,
    pub config_value: u8,
    pub i_config: u8,
    pub bm_attributes: u8,
    pub max_power: u8,
}

impl ConfigDescriptor {
    pub const SIZE: usize = 9;

    pub fn parse(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < Self::SIZE {
            return None;
        }
        Some(Self {
            total_length: u16_at(bytes, 2),
            num_interfaces: bytes[4],
            config_value: bytes[5],
            i_config: bytes[6],
            bm_attributes: bytes[7],
            max_power: bytes[8],
        })
    }
}

#[derive(Debug, Clone, Copy, Default)]
pub struct InterfaceDescriptor {
    pub interface_number: u8,
    pub alternate_setting: u8,
    pub num_endpoints: u8,
    pub interface_class: u8,
    pub interface_subclass: u8,
    pub interface_protocol: u8,
    pub i_interface: u8,
}

impl InterfaceDescriptor {
    pub const SIZE: usize = 9;

    pub fn parse(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < Self::SIZE {
            return None;
        }
        Some(Self {
            interface_number: bytes[2],
            alternate_setting: bytes[3],
            num_endpoints: bytes[4],
            interface_class: bytes[5],
            interface_subclass: bytes[6],
            interface_protocol: bytes[7],
            i_interface: bytes[8],
        })
    }
}

#[derive(Debug, Clone, Copy, Default)]
pub struct EndpointDescriptor {
    pub length: u8,
    pub endpoint_address: u8,
    pub attributes: u8,
    pub max_packet_size: u16,
    pub interval: u8,
}

impl EndpointDescriptor {
    pub const SIZE: usize = 7;

    pub fn parse(bytes: &[u8]) -> Option<Self> {
        if bytes.len() < Self::SIZE {
            return None;
        }
        Some(Self {
            length: bytes[0],
            endpoint_address: bytes[2],
            attributes: bytes[3],
            max_packet_size: u16_at(bytes, 4),
            interval: bytes[6],
        })
    }
}

/// One descriptor of a descriptor buffer: its type and its full bytes.
#[derive(Debug, Clone, Copy)]
pub struct Entry<'a> {
    pub descriptor_type: u8,
    pub bytes: &'a [u8],
}

/// Splits a descriptor buffer into its descriptors. A header that does not fit
/// or a length that over- or underruns the buffer terminates the iteration.
pub fn entries(buffer: &[u8]) -> impl Iterator<Item = Entry<'_>> {
    let mut rest = buffer;
    std::iter::from_fn(move || {
        if rest.len() < 2 {
            return None;
        }
        let length = rest[0] as usize;
        if length < 2 || length > rest.len() {
            rest = &rest[rest.len()..];
            return None;
        }
        let entry = Entry {
            descriptor_type: rest[1],
            bytes: &rest[..length],
        };
        rest = &rest[length..];
        Some(entry)
    })
}

/// The configuration descriptor that a configuration buffer starts with.
pub fn config_descriptor(buffer: &[u8]) -> Option<ConfigDescriptor> {
    let first = entries(buffer).next()?;
    if first.descriptor_type != DESCRIPTOR_TYPE_CONFIGURATION {
        return None;
    }
    ConfigDescriptor::parse(first.bytes)
}

/// An interface descriptor together with the endpoints that follow it.
#[derive(Debug, Clone, Default)]
pub struct Interface {
    pub descriptor: InterfaceDescriptor,
    pub endpoints: Vec<EndpointDescriptor>,
}

/// Groups a configuration buffer by interface descriptor; descriptors before
/// the first interface (the configuration descriptor itself) are skipped.
pub fn interfaces(buffer: &[u8]) -> Vec<Interface> {
    let mut interfaces: Vec<Interface> = Vec::new();
    for entry in entries(buffer) {
        match entry.descriptor_type {
            DESCRIPTOR_TYPE_INTERFACE => interfaces.push(Interface {
                descriptor: InterfaceDescriptor::parse(entry.bytes).unwrap_or_default(),
                endpoints: Vec::new(),
            }),
            DESCRIPTOR_TYPE_ENDPOINT => {
                if let Some(interface) = interfaces.last_mut() {
                    interface
                        .endpoints
                        .push(EndpointDescriptor::parse(entry.bytes).unwrap_or_default());
                }
            }
            _ => {}
        }
    }
    interfaces
}
