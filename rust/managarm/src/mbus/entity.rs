use super::{Error, Item, Properties, Result, bindings};

#[derive(Debug, PartialEq, Eq)]
pub struct Entity {
    id: i64,
}

impl Entity {
    pub fn from_id(id: i64) -> Self {
        Self { id }
    }

    pub async fn get_remote_lane(&self) -> Result<hel::Handle> {
        let head = bragi::head_to_bytes(&bindings::GetRemoteLaneRequest::new(self.id))?;
        let (_offer, (_send_head, recv, pull)) = hel::submit_async(
            crate::posix::mbus_lane_handle(),
            hel::Offer::new((
                hel::SendBuffer::new(&head),
                hel::ReceiveInline,
                hel::PullDescriptor,
            )),
        )
        .await?;

        let recv_data = recv?;
        let response: bindings::GetRemoteLaneResponse = bragi::head_from_bytes(&recv_data)?;

        if response.error() == bindings::Error::Success {
            Ok(pull?.expect("No descriptor pushed"))
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn get_properties(&self) -> Result<Properties> {
        let head = bragi::head_to_bytes(&bindings::GetPropertiesRequest::new(self.id))?;
        let (offer, (_send_head, recv)) = hel::submit_async(
            crate::posix::mbus_lane_handle(),
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

        let response: bindings::GetPropertiesResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        if response.error() == bindings::Error::Success {
            Ok(response
                .properties()
                .iter()
                .map(|property| {
                    let item = Item::decode_item(property.item());
                    let name = property.name().to_string();

                    (name, item)
                })
                .collect())
        } else {
            Err(Error::from(response.error()))
        }
    }

    pub async fn update_properties(&self, properties: Properties) -> Result<()> {
        let request = bindings::UpdatePropertiesRequest::new(
            self.id,
            properties
                .iter()
                .map(|(key, value)| bindings::Property::new(key.clone(), value.encode_item()))
                .collect(),
        );

        let (head, tail) = bragi::head_tail_to_bytes(&request)?;
        let (_offer, (_send_head, _send_tail, recv)) = hel::submit_async(
            crate::posix::mbus_lane_handle(),
            hel::Offer::new_with_lane((
                hel::SendBuffer::new(&head),
                hel::SendBuffer::new(&tail),
                hel::ReceiveInline,
            )),
        )
        .await?;

        let recv_data = recv?;
        let response: bindings::GetRemoteLaneResponse = bragi::head_from_bytes(&recv_data)?;

        if response.error() == bindings::Error::Success {
            Ok(())
        } else {
            Err(Error::from(response.error()))
        }
    }
}
