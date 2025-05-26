pub mod action;
pub mod result;

use std::{
    cell::Cell,
    mem::MaybeUninit,
    rc::Rc,
    task::{LocalWaker, Poll},
    time::Duration,
};

use action::Action;
use result::{FromQueueElement, SimpleResult};

use crate::{
    Time,
    executor::{Executor, ExecutorInner, current_executor},
    handle::Handle,
    queue::QueueElement,
    result::{Result, hel_check},
};

/// Operation state object. This is used to store the submission and completion
/// status alond with any other data that is needed to submit and complete work.
pub struct OperationState<'a> {
    is_submitted: Cell<bool>,
    waker: Cell<Option<LocalWaker>>,
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
    Submit: Fn(&Handle, *const OperationState) -> Result<()>,
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

                if let Err(err) = submit(state.executor.queue_handle(), context) {
                    // In case of an error we need to release the previously
                    // leaked reference to the state object and return the error.
                    drop(unsafe { Rc::from_raw(context) });

                    return Poll::Ready(Err(err));
                }
            }

            // Set the waker for this operation
            state.waker.set(Some(cx.local_waker().clone()));

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
        move |queue_handle, context| {
            hel_check(unsafe {
                hel_sys::helSubmitAwaitClock(
                    time.nanos(),
                    queue_handle.handle(),
                    context as usize,
                    &mut 0, // We don't need the async operation ID
                )
            })
        },
        SimpleResult::from_queue_element,
    )
}

/// Returns a future that will be completed when the system clock
/// reaches the given time value.
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
        move |queue_handle, context| {
            hel_check(unsafe {
                hel_sys::helSubmitAwaitClock(
                    time?.nanos(),
                    queue_handle.handle(),
                    context as usize,
                    &mut 0, // We don't need the async operation ID
                )
            })
        },
        SimpleResult::from_queue_element,
    )
}

/// Returns a future that will be completed after the given duration
/// has passed. This is equivalent to calling `sleep_until` with the
/// current time plus the given duration.
pub fn sleep_for(duration: Duration) -> impl Future<Output = Result<()>> {
    sleep_for_with_executor(current_executor(), duration)
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

    new_async_operation(
        executor.clone_inner(),
        move |queue_handle, context| {
            hel_check(unsafe {
                hel_sys::helSubmitAsync(
                    lane.handle(),
                    actions.as_ptr() as *const _,
                    actions.len(),
                    queue_handle.handle(),
                    context as usize,
                    0,
                )
            })
        },
        |element| Ok(T::Output::from_queue_element(element)),
    )
}

pub fn submit_async<T: Action>(
    lane: &Handle,
    action: T,
) -> impl Future<Output = Result<<T::Output as FromQueueElement>::Output>>
where
    [(); T::ACTION_COUNT]: Sized,
{
    submit_async_with_executor(current_executor(), lane, action)
}
