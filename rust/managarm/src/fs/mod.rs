//! The fs node and passthrough protocols.

pub mod server;

bragi::include_binding!(pub mod bindings = "fs.rs");
