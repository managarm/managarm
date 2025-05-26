use std::mem::MaybeUninit;

use crate::submission::result::{
    FromQueueElement, HandleResult, InlineResult, LengthResult, SimpleResult,
};

const fn default_hel_action() -> hel_sys::HelAction {
    hel_sys::HelAction {
        type_: hel_sys::kHelActionNone as std::ffi::c_int,
        flags: 0,
        buffer: std::ptr::null_mut(),
        length: 0,
        handle: hel_sys::kHelNullHandle as hel_sys::HelHandle,
    }
}

/// Trait for different Hel action types defining how to convert
/// them to FFI HelAction structs.
pub trait Action {
    /// The output type of the action.
    type Output: FromQueueElement;

    /// The number of Hel actions this action converts to.
    const ACTION_COUNT: usize;

    /// Writes the Hel actions to the provided buffer.
    /// The buffer must be able to hold at least [`Self::ACTION_COUNT`] elements.
    fn to_hel_actions(&self, has_next: bool, buffer: &mut [MaybeUninit<hel_sys::HelAction>]);
}

#[doc(hidden)]
macro_rules! impl_action_for_tuple {
    ($([$($arg_name:ident : $gt_name:ident),+])+) => {
        $(
            impl<$($gt_name,)+> Action for ($($gt_name,)+)
            where
                $($gt_name: Action,)+
            {
                type Output = ($($gt_name::Output,)+);

                const ACTION_COUNT: usize = 0 $(+ $gt_name::ACTION_COUNT)+;

                fn to_hel_actions(&self, has_next: bool, buffer: &mut [MaybeUninit<hel_sys::HelAction>]) {
                    let mut index = 0;
                    let ($($arg_name,)+) = self;
                    $(
                        $arg_name.to_hel_actions(
                            if index + $gt_name::ACTION_COUNT < Self::ACTION_COUNT {
                                true
                            } else {
                                has_next
                            },
                            &mut buffer[index..]
                        );
                        index += $gt_name::ACTION_COUNT;
                    )+
                    _ = index;
                }
            }
        )+
    }
}

impl_action_for_tuple! {
    [a0: T0]
    [a0: T0, a1: T1]
    [a0: T0, a1: T1, a2: T2]
    [a0: T0, a1: T1, a2: T2, a3: T3]
    [a0: T0, a1: T1, a2: T2, a3: T3, a4: T4]
    [a0: T0, a1: T1, a2: T2, a3: T3, a4: T4, a5: T5]
    [a0: T0, a1: T1, a2: T2, a3: T3, a4: T4, a5: T5, a6: T6]
    [a0: T0, a1: T1, a2: T2, a3: T3, a4: T4, a5: T5, a6: T6, a7: T7]
    [a0: T0, a1: T1, a2: T2, a3: T3, a4: T4, a5: T5, a6: T6, a7: T7, a8: T8]
    [a0: T0, a1: T1, a2: T2, a3: T3, a4: T4, a5: T5, a6: T6, a7: T7, a8: T8, a9: T9]
    [a0: T0, a1: T1, a2: T2, a3: T3, a4: T4, a5: T5, a6: T6, a7: T7, a8: T8, a9: T9, a10: T10]
}

pub struct Offer<T: Action> {
    want_lane: bool,
    action: T,
}

impl<T: Action> Offer<T> {
    pub fn new(action: T) -> Self {
        Self {
            want_lane: false,
            action,
        }
    }

    pub fn new_with_lane(action: T) -> Self {
        Self {
            want_lane: true,
            action,
        }
    }
}

impl<T: Action> Action for Offer<T> {
    const ACTION_COUNT: usize = T::ACTION_COUNT + 1;

    type Output = (HandleResult, T::Output);

    fn to_hel_actions(&self, has_next: bool, buffer: &mut [MaybeUninit<hel_sys::HelAction>]) {
        let mut action = default_hel_action();

        action.type_ = hel_sys::kHelActionOffer as _;

        if self.want_lane {
            action.flags = hel_sys::kHelItemWantLane;
        }

        if has_next {
            action.flags |= hel_sys::kHelItemChain;
        }

        if T::ACTION_COUNT > 0 {
            action.flags |= hel_sys::kHelItemAncillary;
        }

        buffer[0].write(action);

        self.action.to_hel_actions(false, &mut buffer[1..]);
    }
}

pub struct SendBuffer<'a> {
    data: &'a [u8],
}

impl<'a> SendBuffer<'a> {
    pub fn new(data: &'a [u8]) -> Self {
        Self { data }
    }
}

impl Action for SendBuffer<'_> {
    const ACTION_COUNT: usize = 1;

    type Output = SimpleResult;

    fn to_hel_actions(&self, has_next: bool, buffer: &mut [MaybeUninit<hel_sys::HelAction>]) {
        let mut action = default_hel_action();

        action.type_ = hel_sys::kHelActionSendFromBuffer as _;
        action.buffer = self.data.as_ptr() as *mut _;
        action.length = self.data.len();

        if has_next {
            action.flags = hel_sys::kHelItemChain;
        }

        buffer[0].write(action);
    }
}

pub struct ReceiveBuffer<'a> {
    data: &'a mut [u8],
}

impl<'a> ReceiveBuffer<'a> {
    pub fn new(data: &'a mut [u8]) -> Self {
        Self { data }
    }
}

impl Action for ReceiveBuffer<'_> {
    const ACTION_COUNT: usize = 1;

    type Output = LengthResult;

    fn to_hel_actions(&self, has_next: bool, buffer: &mut [MaybeUninit<hel_sys::HelAction>]) {
        let mut action = default_hel_action();

        action.type_ = hel_sys::kHelActionRecvToBuffer as _;
        action.buffer = self.data.as_ptr() as *mut _;
        action.length = self.data.len();

        if has_next {
            action.flags = hel_sys::kHelItemChain;
        }

        buffer[0].write(action);
    }
}

pub struct ReceiveInline;

impl Action for ReceiveInline {
    const ACTION_COUNT: usize = 1;

    type Output = InlineResult;

    fn to_hel_actions(&self, has_next: bool, buffer: &mut [MaybeUninit<hel_sys::HelAction>]) {
        let mut action = default_hel_action();

        action.type_ = hel_sys::kHelActionRecvInline as _;

        if has_next {
            action.flags = hel_sys::kHelItemChain;
        }

        buffer[0].write(action);
    }
}

pub struct PullDescriptor;

impl Action for PullDescriptor {
    const ACTION_COUNT: usize = 1;

    type Output = HandleResult;

    fn to_hel_actions(&self, has_next: bool, buffer: &mut [MaybeUninit<hel_sys::HelAction>]) {
        let mut action = default_hel_action();

        action.type_ = hel_sys::kHelActionPullDescriptor as _;

        if has_next {
            action.flags = hel_sys::kHelItemChain;
        }

        buffer[0].write(action);
    }
}
