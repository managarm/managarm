use alloc::{borrow::ToOwned, vec::Vec};

use crate::{
    handle::Handle,
    queue::QueueElement,
    result::{Result, hel_check},
};

pub trait FromQueueElement {
    type Output: Sized;

    fn from_queue_element(element: &mut QueueElement) -> Self::Output;
}

macro_rules! impl_from_queue_element_for_tuple {
    ($([$($gt_name:ident),+])+) => {
        $(
            impl<'a, $($gt_name,)+> FromQueueElement for ($($gt_name,)+)
            where
                $($gt_name: FromQueueElement,)+
            {
                type Output = ($($gt_name::Output,)+);

                fn from_queue_element(element: &mut QueueElement) -> Self::Output {
                    ($($gt_name::from_queue_element(element),)+)
                }
            }
        )+
    }
}

impl_from_queue_element_for_tuple! {
    [T0]
    [T0, T1]
    [T0, T1, T2]
    [T0, T1, T2, T3]
    [T0, T1, T2, T3, T4]
    [T0, T1, T2, T3, T4, T5]
    [T0, T1, T2, T3, T4, T5, T6]
    [T0, T1, T2, T3, T4, T5, T6, T7]
    [T0, T1, T2, T3, T4, T5, T6, T7, T8]
    [T0, T1, T2, T3, T4, T5, T6, T7, T8, T9]
    [T0, T1, T2, T3, T4, T5, T6, T7, T8, T9, T10]
}

pub struct SimpleResult;

impl FromQueueElement for SimpleResult {
    type Output = Result<()>;

    fn from_queue_element(element: &mut QueueElement) -> Self::Output {
        let data = element.data();

        assert!(data.len() >= size_of::<hel_sys::HelSimpleResult>());

        // SAFETY: The data is guaranteed to contain enough bytes
        // to read a [`hel_sys::HelSimpleResult`]` and that it is
        // correctly aligned.
        let result = unsafe { data.as_ptr().cast::<hel_sys::HelSimpleResult>().read() };

        element.advance(size_of::<hel_sys::HelSimpleResult>());

        hel_check(result.error).map(|_| ())
    }
}

pub struct LengthResult;

impl FromQueueElement for LengthResult {
    type Output = Result<usize>;

    fn from_queue_element(element: &mut QueueElement) -> Self::Output {
        let data = element.data();

        assert!(data.len() >= size_of::<hel_sys::HelLengthResult>());

        // SAFETY: The data is guaranteed to contain enough bytes
        // to read a [`hel_sys::HelLengthResult`]` and that it is
        // correctly aligned.
        let result = unsafe { data.as_ptr().cast::<hel_sys::HelLengthResult>().read() };

        element.advance(size_of::<hel_sys::HelLengthResult>());

        hel_check(result.error).map(|_| result.length)
    }
}

pub struct EventResult;

impl FromQueueElement for EventResult {
    type Output = Result<u64>;

    fn from_queue_element(element: &mut QueueElement) -> Self::Output {
        let data = element.data();

        assert!(data.len() >= size_of::<hel_sys::HelEventResult>());
        let result = unsafe { data.as_ptr().cast::<hel_sys::HelEventResult>().read() };

        element.advance(size_of::<hel_sys::HelEventResult>());

        hel_check(result.error).map(|_| result.sequence)
    }
}

pub struct HandleResult;

impl FromQueueElement for HandleResult {
    type Output = Result<Option<Handle>>;

    fn from_queue_element(element: &mut QueueElement) -> Self::Output {
        let data = element.data();

        assert!(data.len() >= size_of::<hel_sys::HelHandleResult>());

        // SAFETY: The data is guaranteed to contain enough bytes
        // to read a [`hel_sys::HelHandleResult`]` and that it is
        // correctly aligned.
        let result = unsafe { data.as_ptr().cast::<hel_sys::HelHandleResult>().read() };

        element.advance(size_of::<hel_sys::HelHandleResult>());

        hel_check(result.error).map(|_| {
            if result.handle != hel_sys::kHelNullHandle as hel_sys::HelHandle {
                Some(unsafe { Handle::from_raw(result.handle) })
            } else {
                None
            }
        })
    }
}

pub struct CredentialsResult;

impl FromQueueElement for CredentialsResult {
    type Output = Result<[u8; 16]>;

    fn from_queue_element(element: &mut QueueElement) -> Self::Output {
        let data = element.data();

        assert!(data.len() >= size_of::<hel_sys::HelCredentialsResult>());

        // SAFETY: The data is guaranteed to contain enough bytes
        // to read a [`hel_sys::HelCredentialsResult`]` and that it is
        // correctly aligned.
        let result = unsafe { data.as_ptr().cast::<hel_sys::HelCredentialsResult>().read() };

        element.advance(size_of::<hel_sys::HelCredentialsResult>());

        hel_check(result.error).map(|_| result.credentials.map(|c| c as u8))
    }
}

pub struct InlineResult;

impl FromQueueElement for InlineResult {
    type Output = Result<Vec<u8>>;

    fn from_queue_element(element: &mut QueueElement) -> Self::Output {
        let data = element.data();

        assert!(data.len() >= size_of::<hel_sys::HelInlineResult>());

        // SAFETY: The data is guaranteed to contain enough bytes
        // to read a [`hel_sys::HelInlineResult`]` and that it is
        // correctly aligned.
        let result = unsafe { data.as_ptr().cast::<hel_sys::HelInlineResult>().read() };

        let inline_data = data[size_of::<hel_sys::HelInlineResult>()..]
            .get(..result.length)
            .expect("Inline data length out of bounds");

        // SAFETY: The data after the [`hel_sys::HelInlineResult`] is guaranteed
        // to be valid for up to `result.length` bytes.
        let inline_data = unsafe {
            core::slice::from_raw_parts(inline_data.as_ptr() as *const u8, inline_data.len())
        };

        element.advance(size_of::<hel_sys::HelInlineResult>());

        // Skip the inline data
        element.advance(result.length.next_multiple_of(8));

        hel_check(result.error).map(|_| inline_data.to_owned())
    }
}

/// A request issued by the kernel on the backing handle of a managed memory object
/// or swap space. It is obtained through [`crate::submission::manage_memory`].
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ManageRequest {
    /// The range needs to be filled with its contents, e.g. read in from disk.
    Initialize { offset: usize, length: usize },
    /// The range has been modified and needs to be written out, e.g. to disk.
    Writeback { offset: usize, length: usize },
}

impl ManageRequest {
    /// Returns the byte offset of the requested range within the memory object.
    pub fn offset(&self) -> usize {
        match self {
            ManageRequest::Initialize { offset, .. } => *offset,
            ManageRequest::Writeback { offset, .. } => *offset,
        }
    }

    /// Returns the length of the requested range in bytes.
    pub fn length(&self) -> usize {
        match self {
            ManageRequest::Initialize { length, .. } => *length,
            ManageRequest::Writeback { length, .. } => *length,
        }
    }
}

pub struct ManageResult;

impl FromQueueElement for ManageResult {
    type Output = Result<ManageRequest>;

    fn from_queue_element(element: &mut QueueElement) -> Self::Output {
        let data = element.data();

        assert!(data.len() >= size_of::<hel_sys::HelManageResult>());

        // SAFETY: The data is guaranteed to contain enough bytes
        // to read a [`hel_sys::HelManageResult`]` and that it is
        // correctly aligned.
        let result = unsafe { data.as_ptr().cast::<hel_sys::HelManageResult>().read() };

        element.advance(size_of::<hel_sys::HelManageResult>());

        hel_check(result.error).map(|_| match result.type_ as hel_sys::HelManageRequests {
            hel_sys::kHelManageInitialize => ManageRequest::Initialize {
                offset: result.offset,
                length: result.length,
            },
            hel_sys::kHelManageWriteback => ManageRequest::Writeback {
                offset: result.offset,
                length: result.length,
            },
            _ => unreachable!(),
        })
    }
}
