//! Helpers for publishing and serving mbus entities.

use managarm::mbus::{EntityManager, Item};

pub fn string(value: &str) -> Item {
    Item::String(value.to_string())
}

pub fn hex(value: u32, width: usize) -> Item {
    Item::String(format!("{value:0width$x}"))
}

pub fn decimal(value: i64) -> Item {
    Item::String(format!("{value}"))
}

/// Keeps stream lanes queued on the entity so that each connecting client obtains one.
pub async fn serve_entity_lanes(manager: &'static EntityManager, serve_lane: impl Fn(hel::Handle)) {
    let id = manager.id();

    loop {
        let (local, remote) = match hel::create_stream() {
            Ok(pair) => pair,
            Err(err) => {
                println!("sif: entity {id}: create_stream failed: {err}");
                return;
            }
        };
        if let Err(err) = manager.serve_remote_lane(remote).await {
            println!("sif: entity {id}: serve_remote_lane failed: {err}");
            return;
        }
        serve_lane(local);
    }
}
