use hel::Handle;

use super::{Error, Result, bindings, pci::PciInfo};

pub struct Device {
    handle: Handle,
}

impl Device {
    pub fn new(handle: Handle) -> Self {
        Self { handle }
    }

    pub async fn get_pci_info(&self) -> Result<PciInfo> {
        let head = bragi::head_to_bytes(&bindings::GetPciInfoRequest::new())?;
        let (offer, (_send_head, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let preamble = bragi::preamble_from_bytes(&recv_data)?;
        let mut tail_buffer = vec![0; preamble.tail_size() as usize];

        hel::submit_async(
            &conversation_lane,
            hel::ReceiveBuffer::new(&mut tail_buffer),
        )
        .await??; // Handle both the submit_async and receive errors

        let response: bindings::SvrResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Errors::Success {
            Ok(PciInfo::decode(&response))
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn access_bar(&self, bar: usize) -> Result<Handle> {
        let head = bragi::head_to_bytes(&bindings::AccessBarRequest::new(bar as i32))?;
        let (offer, (_send, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let preamble = bragi::preamble_from_bytes(&recv_data)?;
        let mut tail_buffer = vec![0; preamble.tail_size() as usize];
        let (_recv, pull) = hel::submit_async(
            &conversation_lane,
            (
                hel::ReceiveBuffer::new(&mut tail_buffer),
                hel::PullDescriptor,
            ),
        )
        .await?;

        let response: bindings::SvrResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Errors::Success {
            Ok(pull?.expect("No descriptor pushed"))
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn access_irq(&self, index: usize) -> Result<Handle> {
        let head = bragi::head_to_bytes(&bindings::AccessIrqRequest::new(index as u64))?;
        let (offer, (_send_head, recv)) = hel::submit_async(
            &self.handle,
            hel::Offer::new_with_lane((hel::SendBuffer::new(&head), hel::ReceiveInline)),
        )
        .await?;

        let recv_data = recv?;
        let conversation_lane = offer?.expect("No lane offered");
        let preamble = bragi::preamble_from_bytes(&recv_data)?;
        let mut tail_buffer = vec![0; preamble.tail_size() as usize];
        let (_recv, pull) = hel::submit_async(
            &conversation_lane,
            (
                hel::ReceiveBuffer::new(&mut tail_buffer),
                hel::PullDescriptor,
            ),
        )
        .await?;

        let response: bindings::SvrResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Errors::Success {
            Ok(pull?.expect("No descriptor pushed"))
        } else {
            Err(Error::from(response.error()))
        }
    }
}
