//! Client side of the usb protocol: descriptor retrieval and control
//! transfers against a device that a host controller driver serves.

pub mod descriptor;
pub mod error;

use hel::Handle;

pub use error::Error;

pub type Result<T> = std::result::Result<T, Error>;

bragi::include_binding!(mod bindings = "usb.rs");

const SETUP_TYPE_TO_HOST: u8 = 0x80;
const REQUEST_GET_DESCRIPTOR: u8 = 0x06;
const REQUEST_GET_CONFIGURATION: u8 = 0x08;
const LANGUAGE_EN_US: u16 = 0x0409;

/// The 8-byte setup packet of a control transfer.
#[derive(Debug, Clone, Copy)]
pub struct SetupPacket {
    pub request_type: u8,
    pub request: u8,
    pub value: u16,
    pub index: u16,
    pub length: u16,
}

impl SetupPacket {
    fn to_bytes(&self) -> [u8; 8] {
        let mut bytes = [0u8; 8];
        bytes[0] = self.request_type;
        bytes[1] = self.request;
        bytes[2..4].copy_from_slice(&self.value.to_le_bytes());
        bytes[4..6].copy_from_slice(&self.index.to_le_bytes());
        bytes[6..8].copy_from_slice(&self.length.to_le_bytes());
        bytes
    }
}

fn check(error: bindings::Errors) -> Result<()> {
    if error == bindings::Errors::Success {
        Ok(())
    } else {
        Err(Error::from(error))
    }
}

pub struct Device {
    lane: Handle,
}

impl Device {
    pub fn new(lane: Handle) -> Self {
        Self { lane }
    }

    /// Returns the raw device descriptor.
    pub async fn device_descriptor(&self) -> Result<Vec<u8>> {
        let head = bragi::head_to_bytes(&bindings::GetDeviceDescriptorRequest::new())?;
        let (_offer, (send_head, recv_resp, recv_data)) = hel::submit_async(
            &self.lane,
            hel::Offer::new((
                hel::SendBuffer::new(&head),
                hel::ReceiveInline,
                hel::ReceiveInline,
            )),
        )
        .await?;
        send_head?;
        let resp: bindings::SvrResponse = bragi::head_from_bytes(&recv_resp?)?;
        check(resp.error())?;
        Ok(recv_data?)
    }

    /// Returns the raw configuration descriptor with the given index,
    /// including all interface and endpoint descriptors that follow it.
    pub async fn configuration_descriptor(&self, index: u8) -> Result<Vec<u8>> {
        let head = bragi::head_to_bytes(&bindings::GetConfigurationDescriptorRequest::new(index))?;
        let (offer, (send_head, recv_resp)) = hel::submit_async(
            &self.lane,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;
        send_head?;
        let resp: bindings::SvrResponse = bragi::head_from_bytes(&recv_resp?)?;
        let conversation = offer?.ok_or(Error::Malformed)?;

        // The data is sent even on error, so it has to be received before failing.
        let mut data = vec![0u8; resp.size().unwrap_or(0) as usize];
        let recv_data =
            hel::submit_async(&conversation, hel::ReceiveBuffer::new(&mut data)).await?;
        check(resp.error())?;
        recv_data?;
        Ok(data)
    }

    /// Issues a device-to-host control transfer and returns the received data.
    pub async fn control_transfer_to_host(&self, setup: &SetupPacket) -> Result<Vec<u8>> {
        let req = bindings::TransferRequest::new(
            bindings::XferDirection::ToHost,
            bindings::XferType::Control,
            setup.length as u64,
        );
        let head = bragi::head_to_bytes(&req)?;
        let setup_bytes = setup.to_bytes();
        let mut data = vec![0u8; setup.length as usize];

        let (_offer, (send_head, send_setup, recv_resp, recv_data)) = hel::submit_async(
            &self.lane,
            hel::Offer::new((
                hel::SendBuffer::new(&head),
                hel::SendBuffer::new(&setup_bytes),
                hel::ReceiveInline,
                hel::ReceiveBuffer::new(&mut data),
            )),
        )
        .await?;
        send_head?;
        send_setup?;
        let resp: bindings::SvrResponse = bragi::head_from_bytes(&recv_resp?)?;
        check(resp.error())?;
        recv_data?;

        let size = resp.size().unwrap_or(data.len() as i64) as usize;
        data.truncate(size.min(data.len()));
        Ok(data)
    }

    /// Returns the bConfigurationValue of the active configuration (GET_CONFIGURATION).
    pub async fn current_configuration_value(&self) -> Result<u8> {
        let data = self
            .control_transfer_to_host(&SetupPacket {
                request_type: SETUP_TYPE_TO_HOST,
                request: REQUEST_GET_CONFIGURATION,
                value: 0,
                index: 0,
                length: 1,
            })
            .await?;
        data.first().copied().ok_or(Error::Malformed)
    }

    /// Returns the string descriptor with the given index, decoded from UTF-16.
    /// Index 0 has no string and fails with [`Error::Unsupported`].
    pub async fn string(&self, index: u8) -> Result<String> {
        if index == 0 {
            return Err(Error::Unsupported);
        }
        let mut setup = SetupPacket {
            request_type: SETUP_TYPE_TO_HOST,
            request: REQUEST_GET_DESCRIPTOR,
            value: (descriptor::DESCRIPTOR_TYPE_STRING as u16) << 8 | index as u16,
            index: LANGUAGE_EN_US,
            length: 2,
        };
        // The header carries the total length; fetch that in a second transfer.
        let header = self.control_transfer_to_host(&setup).await?;
        let length = *header.first().ok_or(Error::Malformed)?;
        if length < 2 {
            return Err(Error::Malformed);
        }
        setup.length = length as u16;
        let data = self.control_transfer_to_host(&setup).await?;
        if data.len() < 2 {
            return Err(Error::Malformed);
        }

        let units: Vec<u16> = data[2..]
            .chunks_exact(2)
            .map(|chunk| u16::from_le_bytes([chunk[0], chunk[1]]))
            .collect();
        Ok(String::from_utf16_lossy(&units))
    }
}
