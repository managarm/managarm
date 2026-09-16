//! The tty class subsystem: a static tty0 device.
//!
//! systemd-logind reads /sys/class/tty/tty0/active to track the foreground
//! VT; the /dev/tty* nodes themselves stay in posix.

use std::sync::Arc;

use anyhow::Result;

use crate::device::{DevNode, DevNodeSpec, DeviceSpec, Membership, Model, NodeType, Parent};
use crate::subsystem::attr;

const TTY_MAJOR: i32 = 4;
const TTY0_MINOR: i32 = 0;

pub async fn run(model: Arc<Model>) -> Result<()> {
    let spec = DeviceSpec::new("tty0", Parent::None, Membership::class("tty"))
        .devnode(DevNodeSpec::Foreign(DevNode {
            path: "tty0".to_string(),
            node_type: NodeType::Char,
            major: TTY_MAJOR,
            minor: TTY0_MINOR,
        }))
        .attrs(vec![attr("active", "tty1\n")]);
    model.create_and_announce(spec).await?.persist();
    Ok(())
}
