use super::{Entity, Error, Properties, Result, bindings};

/// Handle to an mbus entity that this process created and manages.
pub struct EntityManager {
    id: i64,
    mgmt_lane: hel::Handle,
}

impl EntityManager {
    pub fn id(&self) -> i64 {
        self.id
    }

    pub fn entity(&self) -> Entity {
        Entity::from_id(self.id)
    }

    /// Attaches a lane to the entity so that clients calling `get_remote_lane` connect to it.
    pub async fn serve_remote_lane(&self, lane: hel::Handle) -> Result<()> {
        let head = bragi::head_to_bytes(&bindings::ServeRemoteLaneRequest::new())?;
        let (_offer, (_send_head, _push_lane, recv)) = hel::submit_async(
            &self.mgmt_lane,
            hel::Offer::new((
                hel::SendBuffer::new(&head),
                hel::PushDescriptor::new(&lane),
                hel::ReceiveInline,
            )),
        )
        .await?;

        let recv_data = recv?;
        let response: bindings::ServeRemoteLaneResponse = bragi::head_from_bytes(&recv_data)?;

        if response.error() == bindings::Error::Success {
            Ok(())
        } else {
            Err(Error::from(response.error()))
        }
    }
}

/// Creates a new mbus entity with the given name and properties.
pub async fn create_entity(name: &str, properties: &Properties) -> Result<EntityManager> {
    let request = bindings::CreateObjectRequest::new(
        name.to_string(),
        properties
            .iter()
            .map(|(key, value)| bindings::Property::new(key.clone(), value.encode_item()))
            .collect(),
    );

    let (head, tail) = bragi::head_tail_to_bytes(&request)?;
    let (_offer, (_send_head, _send_tail, recv, pull)) = hel::submit_async(
        crate::posix::mbus_lane_handle(),
        hel::Offer::new((
            hel::SendBuffer::new(&head),
            hel::SendBuffer::new(&tail),
            hel::ReceiveInline,
            hel::PullDescriptor,
        )),
    )
    .await?;

    let recv_data = recv?;
    let response: bindings::CreateObjectResponse = bragi::head_from_bytes(&recv_data)?;

    if response.error() == bindings::Error::Success {
        Ok(EntityManager {
            id: response.id(),
            mgmt_lane: pull?.expect("No descriptor pushed"),
        })
    } else {
        Err(Error::from(response.error()))
    }
}
