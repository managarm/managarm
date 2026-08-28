use std::ffi::{CStr, c_char, c_void};

use uacpi_sys::{
    uacpi_iteration_callback, uacpi_iteration_decision, uacpi_namespace_node,
    uacpi_predefined_namespace, uacpi_u32, uacpi_u64,
};

use super::{Result, check, check_optional};

/// A node that uACPI predefines, i.e., that always exists.
#[derive(Clone, Copy)]
pub enum PredefinedNamespace {
    /// The system bus, i.e., \_SB.
    SystemBus,
}

impl PredefinedNamespace {
    fn to_raw(self) -> uacpi_predefined_namespace {
        match self {
            PredefinedNamespace::SystemBus => uacpi_sys::UACPI_PREDEFINED_NAMESPACE_SB,
        }
    }
}

/// Determines how iteration over the namespace continues.
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum IterationDecision {
    Continue,
    NextPeer,
    Break,
}

impl IterationDecision {
    fn to_raw(self) -> uacpi_iteration_decision {
        match self {
            IterationDecision::Continue => uacpi_sys::UACPI_ITERATION_DECISION_CONTINUE,
            IterationDecision::NextPeer => uacpi_sys::UACPI_ITERATION_DECISION_NEXT_PEER,
            IterationDecision::Break => uacpi_sys::UACPI_ITERATION_DECISION_BREAK,
        }
    }
}

/// A node of the ACPI namespace.
///
/// Nodes are owned by uACPI and live for as long as the namespace itself.
#[derive(Clone, Copy, PartialEq, Eq)]
pub struct NamespaceNode {
    node: *mut uacpi_namespace_node,
}

// Nodes are only handed back to uACPI, which does its own locking.
unsafe impl Send for NamespaceNode {}
unsafe impl Sync for NamespaceNode {}

impl NamespaceNode {
    /// Wraps uacpi_namespace_root().
    pub fn root() -> NamespaceNode {
        // SAFETY: uACPI does not take any arguments that we could get wrong.
        let node = unsafe { uacpi_sys::uacpi_namespace_root() };
        NamespaceNode::from_raw(node).expect("uACPI lacks a root namespace node")
    }

    /// Wraps uacpi_namespace_get_predefined().
    pub fn predefined(which: PredefinedNamespace) -> NamespaceNode {
        // SAFETY: uACPI does not take any arguments that we could get wrong.
        let node = unsafe { uacpi_sys::uacpi_namespace_get_predefined(which.to_raw()) };
        NamespaceNode::from_raw(node).expect("uACPI lacks a predefined namespace node")
    }

    /// Wraps uacpi_namespace_node_generate_absolute_path().
    pub fn absolute_path(self) -> String {
        // SAFETY: uACPI allocates the path string; we free it below.
        let path = unsafe { uacpi_sys::uacpi_namespace_node_generate_absolute_path(self.node) };
        assert!(!path.is_null(), "uACPI failed to generate an absolute path");
        let string = unsafe { CStr::from_ptr(path) }
            .to_string_lossy()
            .into_owned();
        unsafe { uacpi_sys::uacpi_free_absolute_path(path) };
        string
    }

    /// Wraps uacpi_eval_hid(). Returns None if the device has no _HID.
    pub fn eval_hid(self) -> Result<Option<String>> {
        let mut id: *mut uacpi_sys::uacpi_id_string = std::ptr::null_mut();
        // SAFETY: uACPI only writes the pointer to the id string that it allocates.
        let status = unsafe { uacpi_sys::uacpi_eval_hid(self.node, &mut id) };
        if !check_optional("uacpi_eval_hid", status)? {
            return Ok(None);
        }

        // SAFETY: uACPI hands out a valid id string; we free it below.
        let value = unsafe { CStr::from_ptr((*id).value) }
            .to_string_lossy()
            .into_owned();
        unsafe { uacpi_sys::uacpi_free_id_string(id) };
        Ok(Some(value))
    }

    /// Wraps uacpi_eval_cid(). Returns None if the device has no _CID.
    pub fn eval_cid(self) -> Result<Option<Vec<String>>> {
        let mut list: *mut uacpi_sys::uacpi_pnp_id_list = std::ptr::null_mut();
        // SAFETY: uACPI only writes the pointer to the id list that it allocates.
        let status = unsafe { uacpi_sys::uacpi_eval_cid(self.node, &mut list) };
        if !check_optional("uacpi_eval_cid", status)? {
            return Ok(None);
        }

        // SAFETY: uACPI hands out a list of num_ids valid id strings; we free it below.
        let values = unsafe {
            (*list)
                .ids
                .as_slice((*list).num_ids as usize)
                .iter()
                .map(|id| CStr::from_ptr(id.value).to_string_lossy().into_owned())
                .collect()
        };
        unsafe { uacpi_sys::uacpi_free_pnp_id_list(list) };
        Ok(Some(values))
    }

    pub(super) fn from_raw(node: *mut uacpi_namespace_node) -> Option<NamespaceNode> {
        (!node.is_null()).then_some(NamespaceNode { node })
    }

    pub(super) fn as_raw(self) -> *mut uacpi_namespace_node {
        self.node
    }

    /// Wraps uacpi_eval_simple_integer(). Returns None if the method does not exist.
    pub fn eval_simple_integer(self, path: &CStr) -> Result<Option<u64>> {
        let mut value: uacpi_u64 = 0;
        // SAFETY: uACPI only reads the path and only writes to value.
        let status =
            unsafe { uacpi_sys::uacpi_eval_simple_integer(self.node, path.as_ptr(), &mut value) };
        Ok(check_optional("uacpi_eval_simple_integer", status)?.then_some(value))
    }

    /// Wraps uacpi_eval_adr(). Returns None if the device has no _ADR.
    pub fn eval_adr(self) -> Result<Option<u64>> {
        let mut value: uacpi_u64 = 0;
        // SAFETY: uACPI only writes to value.
        let status = unsafe { uacpi_sys::uacpi_eval_adr(self.node, &mut value) };
        Ok(check_optional("uacpi_eval_adr", status)?.then_some(value))
    }

    /// Wraps uacpi_namespace_for_each_child(), restricted to device nodes at any depth.
    pub fn for_each_child_device<F>(self, mut f: F) -> Result<()>
    where
        F: FnMut(NamespaceNode) -> IterationDecision,
    {
        let (callback, user) = as_iteration_callback(&mut f);
        // SAFETY: the callback and its user pointer match and outlive the call.
        let status = unsafe {
            uacpi_sys::uacpi_namespace_for_each_child(
                self.node,
                callback,
                None,
                uacpi_sys::UACPI_OBJECT_DEVICE_BIT,
                uacpi_sys::UACPI_MAX_DEPTH_ANY,
                user,
            )
        };
        check("uacpi_namespace_for_each_child", status)
    }
}

/// Wraps uacpi_find_devices_at(), i.e., calls f() for each present device below parent
/// whose _HID or _CID matches any of hids.
pub fn find_devices_at<F>(parent: NamespaceNode, hids: &[&CStr], mut f: F) -> Result<()>
where
    F: FnMut(NamespaceNode) -> IterationDecision,
{
    // uACPI expects the list of IDs to be terminated by a null pointer.
    let mut raw_hids: Vec<*const c_char> = hids.iter().map(|hid| hid.as_ptr()).collect();
    raw_hids.push(std::ptr::null());

    let (callback, user) = as_iteration_callback(&mut f);
    // SAFETY: uACPI only reads the IDs; the callback and its user pointer match and
    //         outlive the call.
    let status = unsafe {
        uacpi_sys::uacpi_find_devices_at(parent.as_raw(), raw_hids.as_ptr(), callback, user)
    };
    check("uacpi_find_devices_at", status)
}

/// Turns a closure into the callback and user pointer pair that uACPI expects.
fn as_iteration_callback<F>(f: &mut F) -> (uacpi_iteration_callback, *mut c_void)
where
    F: FnMut(NamespaceNode) -> IterationDecision,
{
    (Some(iteration_trampoline::<F>), f as *mut F as *mut c_void)
}

unsafe extern "C" fn iteration_trampoline<F>(
    user: *mut c_void,
    node: *mut uacpi_namespace_node,
    _depth: uacpi_u32,
) -> uacpi_iteration_decision
where
    F: FnMut(NamespaceNode) -> IterationDecision,
{
    // SAFETY: user points to the closure that as_iteration_callback() handed to uACPI.
    let f = unsafe { &mut *(user as *mut F) };
    let node = NamespaceNode::from_raw(node).expect("uACPI visited a null namespace node");
    f(node).to_raw()
}
