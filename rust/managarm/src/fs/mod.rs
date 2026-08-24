//! The fs passthrough protocol.

pub mod server;

bragi::include_binding!(pub mod bindings = "fs.rs");
