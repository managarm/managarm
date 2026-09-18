use uacpi_sys::{uacpi_gpe_handler, uacpi_handle, uacpi_notify_handler, uacpi_region_handler};

use super::namespace::NamespaceNode;
use super::{Result, check};

type GpeDevice = Option<NamespaceNode>;

fn as_gpe_device(device: GpeDevice) -> *mut uacpi_sys::uacpi_namespace_node {
    device
        .map(NamespaceNode::as_raw)
        .unwrap_or(std::ptr::null_mut())
}

pub fn install_address_space_handler(
    device: NamespaceNode,
    space: uacpi_sys::uacpi_address_space,
    handler: uacpi_region_handler,
    context: uacpi_handle,
) -> Result<()> {
    unsafe {
        check(
            "uacpi_install_address_space_handler",
            uacpi_sys::uacpi_install_address_space_handler(
                device.as_raw(),
                space,
                handler,
                context,
            ),
        )
    }
}

pub fn install_notify_handler(
    node: NamespaceNode,
    handler: uacpi_notify_handler,
    context: uacpi_handle,
) -> Result<()> {
    unsafe {
        check(
            "uacpi_install_notify_handler",
            uacpi_sys::uacpi_install_notify_handler(node.as_raw(), handler, context),
        )
    }
}

pub fn install_gpe_handler(
    device: GpeDevice,
    index: u16,
    triggering: uacpi_sys::uacpi_gpe_triggering,
    handler: uacpi_gpe_handler,
    context: uacpi_handle,
) -> Result<()> {
    let device = as_gpe_device(device);

    unsafe {
        check(
            "uacpi_install_gpe_handler",
            uacpi_sys::uacpi_install_gpe_handler(device, index, triggering, handler, context),
        )
    }
}

pub fn finish_handling_gpe(device: GpeDevice, index: u16) -> Result<()> {
    let device = as_gpe_device(device);

    unsafe {
        check(
            "uacpi_finish_handling_gpe",
            uacpi_sys::uacpi_finish_handling_gpe(device, index),
        )
    }
}

pub fn finalize_gpe_initialization() -> Result<()> {
    unsafe {
        check(
            "uacpi_finalize_gpe_initialization",
            uacpi_sys::uacpi_finalize_gpe_initialization(),
        )
    }
}

pub fn enable_gpe(device: GpeDevice, index: u16) -> Result<()> {
    let device = as_gpe_device(device);

    unsafe {
        check(
            "uacpi_enable_gpe",
            uacpi_sys::uacpi_enable_gpe(device, index),
        )
    }
}
