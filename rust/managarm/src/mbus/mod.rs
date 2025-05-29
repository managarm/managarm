pub mod entity;
pub mod error;
pub mod result;

use std::collections::{HashMap, HashSet};

pub use entity::Entity;
pub use error::Error;
pub use result::Result;

bragi::include_binding!(mod bindings = "mbus.rs");

type Properties = HashMap<String, Item>;

#[derive(Debug, Clone)]
pub enum Item {
    String(String),
    Array(Vec<Item>),
}

impl Item {
    fn encode_item(&self) -> bindings::AnyItem {
        let mut item = bindings::AnyItem::default();

        match self {
            Self::String(string) => {
                item.set_type(bindings::ItemType::String);
                item.set_string_item(string.clone());
            }
            Self::Array(array) => {
                item.set_type(bindings::ItemType::Array);
                item.set_items(array.iter().map(|item| item.encode_item()).collect());
            }
        }

        item
    }

    fn decode_item(item: &bindings::AnyItem) -> Self {
        match item.type_() {
            bindings::ItemType::String => {
                Self::String(item.string_item().expect("String item expected").into())
            }
            bindings::ItemType::Array => Self::Array(
                item.items()
                    .expect("Array item expected")
                    .iter()
                    .map(Self::decode_item)
                    .collect(),
            ),
        }
    }
}

#[derive(Debug, Clone, Copy)]
pub enum Filter<'a, 'b, 'c> {
    Equals(&'a str, &'b str),
    Conjunction(&'c [Filter<'a, 'b, 'c>]),
    Disjunction(&'c [Filter<'a, 'b, 'c>]),
}

impl Filter<'_, '_, '_> {
    /// Converts the filter into a [`bindings::AnyFilter`] so it can be sent over IPC.
    fn encode_filter(&self) -> bindings::AnyFilter {
        let mut filter = bindings::AnyFilter::default();

        match self {
            Self::Equals(path, value) => {
                filter.set_type(bindings::FilterType::Equals);
                filter.set_path(path.to_string());
                filter.set_value(value.to_string());
            }
            Self::Conjunction(filters) => {
                filter.set_type(bindings::FilterType::Conjunction);
                filter.set_operands(filters.iter().map(|f| f.encode_filter()).collect());
            }
            Self::Disjunction(filters) => {
                filter.set_type(bindings::FilterType::Disjunction);
                filter.set_operands(filters.iter().map(|f| f.encode_filter()).collect());
            }
        }

        filter
    }
}

/// Represents the type of event that can be received from mbus.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EventType {
    /// The entity was created.
    Created,
    /// The entity was removed.
    Removed,
    /// The entity's properties were changed.
    PropertiesChanged,
}

/// Represents an event received from mbus.
#[derive(Debug)]
pub struct EnumerationEvent {
    event_type: EventType,
    entity_id: i64,
    name: String,
    properties: Properties,
}

impl EnumerationEvent {
    /// Returns the type of event.
    pub fn event_type(&self) -> EventType {
        self.event_type
    }

    /// Returns the ID of the entity this event relates to.
    pub fn entity_id(&self) -> i64 {
        self.entity_id
    }

    /// Returns the entity this event relates to.
    /// This is a convenience method that creates an [`Entity`] object.
    pub fn entity(&self) -> Entity {
        Entity::from_id(self.entity_id)
    }

    /// Returns the name of the entity this event relates to.
    pub fn name(&self) -> &str {
        &self.name
    }

    /// Returns the properties of the entity this event relates to.
    /// If the entity was created, this will contain the initial properties.
    /// If the entity's properties were changed, this will contain the changed properties.
    pub fn properties(&self) -> &Properties {
        &self.properties
    }
}

pub struct Enumerator {
    filter: bindings::AnyFilter,
    current_seq: u64,
    seen_ids: HashSet<i64>,
}

impl Enumerator {
    pub fn new(filter: Filter) -> Self {
        Self {
            filter: filter.encode_filter(),
            current_seq: 0,
            seen_ids: HashSet::new(),
        }
    }

    pub async fn next_events(&mut self) -> Result<(bool, Vec<EnumerationEvent>)> {
        let request = bindings::EnumerateRequest::new(self.current_seq, self.filter.clone());

        let (head, tail) = bragi::head_tail_to_bytes(&request)?;
        let (offer, (_send_head, _send_tail, recv)) = hel::submit_async(
            crate::posix::mbus_lane_handle(),
            hel::Offer::new_with_lane((
                hel::SendBuffer::new(&head),
                hel::SendBuffer::new(&tail),
                hel::ReceiveInline,
            )),
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

        let response: bindings::EnumerateResponse =
            bragi::head_tail_from_bytes(&recv_data, &tail_buffer)?;

        self.current_seq = response.out_seq();

        Ok((
            response.out_seq() != response.actual_seq(),
            response
                .entities()
                .iter()
                .map(|entity| {
                    let first_seen = self.seen_ids.insert(entity.id());

                    EnumerationEvent {
                        event_type: if first_seen {
                            EventType::Created
                        } else {
                            EventType::PropertiesChanged
                        },
                        entity_id: entity.id(),
                        name: entity.name().to_string(),
                        properties: entity
                            .properties()
                            .iter()
                            .map(|property| {
                                let item = Item::decode_item(property.item());
                                let name = property.name().to_string();

                                (name, item)
                            })
                            .collect(),
                    }
                })
                .collect(),
        ))
    }
}
