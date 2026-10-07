use uacpi_sys::{
    uacpi_handle, uacpi_interrupt_ret, uacpi_namespace_node, uacpi_region_op, uacpi_region_rw_data,
    uacpi_status, uacpi_u16, uacpi_u64,
};

use super::namespace::NamespaceNode;
use super::runtime::{self, Aml};
use super::{Result, check};

type GpeDevice = Option<NamespaceNode>;

fn as_gpe_device(device: GpeDevice) -> *mut uacpi_sys::uacpi_namespace_node {
    device
        .map(NamespaceNode::as_raw)
        .unwrap_or(std::ptr::null_mut())
}

/// Hands a handler to uACPI, which keeps it forever.
fn leak_handler<F>(handler: F) -> uacpi_handle {
    Box::into_raw(Box::new(handler)) as uacpi_handle
}

/// Whether AML reads from or writes to an operation region.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum RegionOp {
    Read,
    Write,
}

/// A read or write of AML to an operation region.
pub struct RegionRw<'a> {
    data: &'a mut uacpi_region_rw_data,
}

impl RegionRw<'_> {
    /// Returns the offset of the access within the address space.
    pub fn offset(&self) -> u64 {
        // SAFETY: the union only holds different names of the same integer.
        unsafe { self.data.__bindgen_anon_1.offset }
    }

    pub fn byte_width(&self) -> u8 {
        self.data.byte_width
    }

    /// Returns the value that AML writes.
    pub fn value(&self) -> u64 {
        self.data.value
    }

    /// Sets the value that AML reads.
    pub fn set_value(&mut self, value: u64) {
        self.data.value = value;
    }
}

/// Reasons why a region handler can fail an access.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum RegionError {
    InvalidArgument,
    HardwareTimeout,
}

impl RegionError {
    fn to_raw(self) -> uacpi_status {
        match self {
            RegionError::InvalidArgument => uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT,
            RegionError::HardwareTimeout => uacpi_sys::UACPI_STATUS_HARDWARE_TIMEOUT,
        }
    }
}

unsafe extern "C" fn region_trampoline<F>(op: uacpi_region_op, data: uacpi_handle) -> uacpi_status
where
    F: Fn(RegionOp, &mut RegionRw<'_>) -> std::result::Result<(), RegionError>,
{
    let op = match op {
        uacpi_sys::UACPI_REGION_OP_ATTACH | uacpi_sys::UACPI_REGION_OP_DETACH => {
            return uacpi_sys::UACPI_STATUS_OK;
        }
        uacpi_sys::UACPI_REGION_OP_READ => RegionOp::Read,
        uacpi_sys::UACPI_REGION_OP_WRITE => RegionOp::Write,
        _ => return uacpi_sys::UACPI_STATUS_INVALID_ARGUMENT,
    };

    // SAFETY: uACPI passes a uacpi_region_rw_data for reads and writes.
    let data = unsafe { &mut *(data as *mut uacpi_region_rw_data) };
    // SAFETY: the context is the handler that install_address_space_handler() leaked.
    let handler = unsafe { &*(data.handler_context as *const F) };
    match handler(op, &mut RegionRw { data }) {
        Ok(()) => uacpi_sys::UACPI_STATUS_OK,
        Err(err) => err.to_raw(),
    }
}

/// Wraps uacpi_install_address_space_handler(), which runs _REG; the handler serves reads
/// and writes.
pub fn install_address_space_handler<F>(
    _aml: Aml,
    device: NamespaceNode,
    space: uacpi_sys::uacpi_address_space,
    handler: F,
) -> Result<()>
where
    F: Fn(RegionOp, &mut RegionRw<'_>) -> std::result::Result<(), RegionError>
        + Send
        + Sync
        + 'static,
{
    unsafe {
        check(
            "uacpi_install_address_space_handler",
            uacpi_sys::uacpi_install_address_space_handler(
                device.as_raw(),
                space,
                Some(region_trampoline::<F>),
                leak_handler(handler),
            ),
        )
    }
}

unsafe extern "C" fn notify_trampoline<F>(
    context: uacpi_handle,
    node: *mut uacpi_namespace_node,
    value: uacpi_u64,
) -> uacpi_status
where
    F: Fn(Aml, NamespaceNode, u64),
{
    // uACPI runs Notify() handlers as work, i.e., on an AML thread.
    let aml = Aml::current().expect("uACPI ran a Notify() handler outside of an AML thread");
    // SAFETY: the context is the handler that install_notify_handler() leaked.
    let handler = unsafe { &*(context as *const F) };
    let node = NamespaceNode::from_raw(node).expect("uACPI notified a null namespace node");
    handler(aml, node, value);
    uacpi_sys::UACPI_STATUS_OK
}

/// Wraps uacpi_install_notify_handler(); the handler receives the value of AML's Notify().
///
/// uACPI blocks while AML runs here (on the namespace lock and on GPE and Notify() work), hence
/// this runs on the acpi thread. A wrapper for uacpi_uninstall_notify_handler() must do the same.
pub async fn install_notify_handler<F>(node: NamespaceNode, handler: F) -> Result<()>
where
    F: Fn(Aml, NamespaceNode, u64) + Send + Sync + 'static,
{
    runtime::run(move |_aml| unsafe {
        check(
            "uacpi_install_notify_handler",
            uacpi_sys::uacpi_install_notify_handler(
                node.as_raw(),
                Some(notify_trampoline::<F>),
                leak_handler(handler),
            ),
        )
    })
    .await
}

unsafe extern "C" fn gpe_trampoline<F>(
    context: uacpi_handle,
    _gpe_device: *mut uacpi_namespace_node,
    _index: uacpi_u16,
) -> uacpi_interrupt_ret
where
    F: Fn(),
{
    // SAFETY: the context is the handler that install_gpe_handler() leaked.
    let handler = unsafe { &*(context as *const F) };
    handler();
    uacpi_sys::UACPI_INTERRUPT_HANDLED
}

/// Wraps uacpi_install_gpe_handler().
///
/// The handler runs in interrupt context, hence it must not run AML. The GPE stays disabled
/// until [`finish_handling_gpe`] is called.
///
/// uACPI waits for GPE and Notify() work here if the GPE is enabled, hence this runs on the
/// acpi thread. A wrapper for uacpi_uninstall_gpe_handler() must do the same.
pub async fn install_gpe_handler<F>(
    device: GpeDevice,
    index: u16,
    triggering: uacpi_sys::uacpi_gpe_triggering,
    handler: F,
) -> Result<()>
where
    F: Fn() + Send + Sync + 'static,
{
    runtime::run(move |_aml| {
        let device = as_gpe_device(device);

        unsafe {
            check(
                "uacpi_install_gpe_handler",
                uacpi_sys::uacpi_install_gpe_handler(
                    device,
                    index,
                    triggering,
                    Some(gpe_trampoline::<F>),
                    leak_handler(handler),
                ),
            )
        }
    })
    .await
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
