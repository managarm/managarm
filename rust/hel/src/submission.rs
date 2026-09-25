pub mod action;
pub mod result;

use alloc::rc::Rc;
use core::{
    cell::Cell,
    mem::{MaybeUninit, size_of},
    task::{Poll, Waker},
    time::Duration,
};

use action::Action;
use result::{EventResult, FromQueueElement, SimpleResult};

#[cfg(feature = "std")]
use crate::executor::current_executor;
use crate::{
    DmaDeviceId, Time,
    executor::{Executor, ExecutorInner},
    handle::Handle,
    queue::QueueElement,
    result::Result,
};

/// Operation state object. This is used to store the submission and completion
/// status alond with any other data that is needed to submit and complete work.
pub struct OperationState<'a> {
    is_submitted: Cell<bool>,
    waker: Cell<Option<Waker>>,
    element: Cell<Option<QueueElement<'a>>>,
    executor: Rc<ExecutorInner>,
}

impl<'a> OperationState<'a> {
    /// Creates a new operation state object.
    fn new(executor: Rc<ExecutorInner>) -> Self {
        Self {
            is_submitted: Cell::new(false),
            waker: Cell::new(None),
            element: Cell::new(None),
            executor,
        }
    }

    /// Returns whether the submission has been submitted to the queue.
    fn is_submitted(&self) -> bool {
        self.is_submitted.get()
    }

    /// Returns the queue element associated with this submission.
    /// This is only valid once the submission has been completed.
    fn queue_element(&self) -> Option<QueueElement<'a>> {
        self.element.take()
    }

    /// Completes the submission and stores the queue element for later use.
    pub(crate) fn complete(&self, element: QueueElement<'a>) {
        self.element.set(Some(element));
        self.waker.take().unwrap().wake();
    }
}

/// Returns a future that will place a new element onto the given queue
/// when first polled and will only be completed once the completion
/// for the element is received.
///
/// If the [`submit`] closure returns a success, at most one
/// queue element must be placed onto the given queue. If an error
/// is returned, no elements may be placed on the given queue.
fn new_async_operation<
    Submit: Fn(&Rc<ExecutorInner>, *const OperationState) -> Result<()>,
    Complete: Fn(&mut QueueElement) -> Result<T>,
    T: Sized,
>(
    executor: Rc<ExecutorInner>,
    submit: Submit,
    complete: Complete,
) -> impl Future<Output = Result<T>> {
    let state = Rc::new(OperationState::new(executor));

    core::future::poll_fn(move |cx| {
        if let Some(mut element) = state.queue_element() {
            // Already completed, parse the result
            Poll::Ready(complete(&mut element))
        } else {
            if !state.is_submitted() {
                // Not submitted yet, submit it

                // Leak a reference to the state object so that it doesn't
                // get dropped in case the future goes out of scope - it will
                // be dropped once the submission is completed.
                let context = Rc::into_raw(state.clone());

                if let Err(err) = submit(&state.executor, context) {
                    // In case of an error we need to release the previously
                    // leaked reference to the state object and return the error.
                    drop(unsafe { Rc::from_raw(context) });

                    return Poll::Ready(Err(err));
                }

                state.is_submitted.set(true);
            }

            // Set the waker for this operation, keeping the stored one if it
            // still wakes the same task (this avoids a reference count update)
            let waker = match state.waker.take() {
                Some(waker) if waker.will_wake(cx.waker()) => waker,
                _ => cx.waker().clone(),
            };
            state.waker.set(Some(waker));

            // Now we can wait for it to finish
            Poll::Pending
        }
    })
}

/// Returns a future that will be completed when the system clock
/// reaches the given time value. The submissions will be placed
/// on the given executor's queue.
pub fn sleep_until_with_executor(
    executor: Executor,
    time: Time,
) -> impl Future<Output = Result<()>> {
    new_async_operation(
        executor.clone_inner(),
        move |executor, context| {
            let header = hel_sys::HelSqAwaitClock {
                counter: time.nanos(),
                cancellationTag: 0, // No cancellation needed
            };
            let header_bytes: &[u8] = unsafe {
                core::slice::from_raw_parts(
                    &header as *const _ as *const u8,
                    size_of::<hel_sys::HelSqAwaitClock>(),
                )
            };
            executor.push_sq(
                hel_sys::kHelSubmitAwaitClock,
                context as usize,
                &[header_bytes],
            )
        },
        SimpleResult::from_queue_element,
    )
}

/// Returns a future that will be completed when the system clock
/// reaches the given time value.
#[cfg(feature = "std")]
pub fn sleep_until(time: Time) -> impl Future<Output = Result<()>> {
    sleep_until_with_executor(current_executor(), time)
}

/// Returns a future that will be completed after the given duration
/// has passed. This is equivalent to calling `sleep_until` with the
/// current time plus the given duration. The submission will be placed
/// on the given executor's queue.
pub fn sleep_for_with_executor(
    executor: Executor,
    duration: Duration,
) -> impl Future<Output = Result<()>> {
    let time = Time::new_since_boot().map(|time| time + duration);

    new_async_operation(
        executor.clone_inner(),
        move |executor, context| {
            let header = hel_sys::HelSqAwaitClock {
                counter: time?.nanos(),
                cancellationTag: 0, // No cancellation needed
            };
            let header_bytes: &[u8] = unsafe {
                core::slice::from_raw_parts(
                    &header as *const _ as *const u8,
                    size_of::<hel_sys::HelSqAwaitClock>(),
                )
            };
            executor.push_sq(
                hel_sys::kHelSubmitAwaitClock,
                context as usize,
                &[header_bytes],
            )
        },
        SimpleResult::from_queue_element,
    )
}

/// Returns a future that will be completed after the given duration
/// has passed. This is equivalent to calling `sleep_until` with the
/// current time plus the given duration.
#[cfg(feature = "std")]
pub fn sleep_for(duration: Duration) -> impl Future<Output = Result<()>> {
    sleep_for_with_executor(current_executor(), duration)
}

pub fn await_event_with_executor(
    executor: Executor,
    event: &Handle,
    sequence: u64,
) -> impl Future<Output = Result<u64>> {
    let handle = event.handle();

    new_async_operation(
        executor.clone_inner(),
        move |executor, context| {
            let header = hel_sys::HelSqAwaitEvent {
                handle,
                sequence,
                cancellationTag: 0,
            };
            let header_bytes: &[u8] = unsafe {
                core::slice::from_raw_parts(
                    &header as *const _ as *const u8,
                    size_of::<hel_sys::HelSqAwaitEvent>(),
                )
            };
            executor.push_sq(
                hel_sys::kHelSubmitAwaitEvent,
                context as usize,
                &[header_bytes],
            )
        },
        EventResult::from_queue_element,
    )
}

#[cfg(feature = "std")]
pub fn await_event(event: &Handle, sequence: u64) -> impl Future<Output = Result<u64>> {
    await_event_with_executor(current_executor(), event, sequence)
}

pub fn submit_async_with_executor<T: Action>(
    executor: Executor,
    lane: &Handle,
    action: T,
) -> impl Future<Output = Result<<T::Output as FromQueueElement>::Output>>
where
    [(); T::ACTION_COUNT]: Sized,
{
    let mut actions = [const { MaybeUninit::uninit() }; T::ACTION_COUNT];

    action.to_hel_actions(false, &mut actions);

    let actions = actions.map(|action| unsafe { action.assume_init() });

    let lane_handle = lane.handle();

    new_async_operation(
        executor.clone_inner(),
        move |executor, context| {
            let header = hel_sys::HelSqExchangeMsgs {
                lane: lane_handle,
                count: actions.len(),
                flags: 0,
            };
            let header_bytes: &[u8] = unsafe {
                core::slice::from_raw_parts(
                    &header as *const _ as *const u8,
                    size_of::<hel_sys::HelSqExchangeMsgs>(),
                )
            };
            let actions_bytes: &[u8] = unsafe {
                core::slice::from_raw_parts(
                    actions.as_ptr() as *const u8,
                    actions.len() * size_of::<hel_sys::HelAction>(),
                )
            };
            executor.push_sq(
                hel_sys::kHelSubmitExchangeMsgs,
                context as usize,
                &[header_bytes, actions_bytes],
            )
        },
        |element| Ok(T::Output::from_queue_element(element)),
    )
}

#[cfg(feature = "std")]
pub fn submit_async<T: Action>(
    lane: &Handle,
    action: T,
) -> impl Future<Output = Result<<T::Output as FromQueueElement>::Output>>
where
    [(); T::ACTION_COUNT]: Sized,
{
    submit_async_with_executor(current_executor(), lane, action)
}

/// Returns a future that submits an SQ element that consists of nothing but its header.
fn new_header_operation<T>(
    executor: Executor,
    opcode: u32,
    header: T,
) -> impl Future<Output = Result<()>> {
    new_async_operation(
        executor.clone_inner(),
        move |executor, context| {
            let header_bytes: &[u8] = unsafe {
                core::slice::from_raw_parts(&header as *const _ as *const u8, size_of::<T>())
            };
            executor.push_sq(opcode, context as usize, &[header_bytes])
        },
        SimpleResult::from_queue_element,
    )
}

/// Returns a future that programs an IOMMU to translate the DMA requests of a device
/// through the given DMA space. The submission is placed on the given executor's queue.
///
/// `None` binds the device in passthrough mode.
pub fn bind_dma_device_with_executor(
    executor: Executor,
    iommu: &Handle,
    space: Option<&Handle>,
    id: DmaDeviceId,
) -> impl Future<Output = Result<()>> {
    new_header_operation(
        executor,
        hel_sys::kHelSubmitBindDmaDevice,
        hel_sys::HelSqBindDmaDevice {
            iommuHandle: iommu.handle(),
            dmaSpaceHandle: space.map_or(hel_sys::kHelNullHandle as hel_sys::HelHandle, |s| {
                s.handle()
            }),
            id: id.to_raw(),
        },
    )
}

/// Returns a future that programs an IOMMU to translate the DMA requests of a device
/// through the given DMA space.
///
/// `None` binds the device in passthrough mode.
#[cfg(feature = "std")]
pub fn bind_dma_device(
    iommu: &Handle,
    space: Option<&Handle>,
    id: DmaDeviceId,
) -> impl Future<Output = Result<()>> {
    bind_dma_device_with_executor(current_executor(), iommu, space, id)
}

/// Returns a future that programs an IOMMU to block the DMA requests of a device.
/// The submission is placed on the given executor's queue.
pub fn unbind_dma_device_with_executor(
    executor: Executor,
    iommu: &Handle,
    id: DmaDeviceId,
) -> impl Future<Output = Result<()>> {
    new_header_operation(
        executor,
        hel_sys::kHelSubmitUnbindDmaDevice,
        hel_sys::HelSqUnbindDmaDevice {
            iommuHandle: iommu.handle(),
            id: id.to_raw(),
        },
    )
}

/// Returns a future that programs an IOMMU to block the DMA requests of a device.
#[cfg(feature = "std")]
pub fn unbind_dma_device(iommu: &Handle, id: DmaDeviceId) -> impl Future<Output = Result<()>> {
    unbind_dma_device_with_executor(current_executor(), iommu, id)
}

/// Returns a future that activates translation on an IOMMU. The submission is placed on
/// the given executor's queue.
///
/// Until this is done, the unit is transparent and every requester DMAs untranslated.
pub fn activate_iommu_with_executor(
    executor: Executor,
    iommu: &Handle,
) -> impl Future<Output = Result<()>> {
    new_header_operation(
        executor,
        hel_sys::kHelSubmitActivateIommu,
        hel_sys::HelSqActivateIommu {
            iommuHandle: iommu.handle(),
        },
    )
}

/// Returns a future that activates translation on an IOMMU.
///
/// Until this is done, the unit is transparent and every requester DMAs untranslated.
#[cfg(feature = "std")]
pub fn activate_iommu(iommu: &Handle) -> impl Future<Output = Result<()>> {
    activate_iommu_with_executor(current_executor(), iommu)
}
