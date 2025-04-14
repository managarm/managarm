use std::{
    cell::Cell,
    rc::Rc,
    task::{LocalWaker, Poll},
    time::Duration,
};

use crate::{Error, Executor, ExecutorInner, Handle, QueueElement, Result, Time, hel_check};

impl TryFrom<QueueElement<'_>> for () {
    type Error = Error;

    fn try_from(element: QueueElement) -> Result<Self> {
        let data = element.data();

        assert!(data.len() >= size_of::<hel_sys::HelSimpleResult>());

        // SAFETY: The data is guaranteed to contain enough bytes
        // to read a [`hel_sys::HelSimpleResult`]` and that it is
        // correctly aligned.
        let result = unsafe { data.as_ptr().cast::<hel_sys::HelSimpleResult>().read() };

        hel_check(result.error).map(|_| ())
    }
}

/// Operation state object. This is used to store the submission and completion
/// status alond with any other data that is needed to submit and complete work.
pub(crate) struct OperationState<'a> {
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
    'a,
    Submit: Fn(&Handle, *const OperationState) -> Result<()>,
    Output: TryFrom<QueueElement<'a>, Error = Error>,
>(
    executor: &Executor,
    submit: Submit,
) -> impl Future<Output = std::result::Result<Output, Output::Error>> {
    let state = Rc::new(OperationState::new(executor.clone_inner()));

    core::future::poll_fn(move |cx| {
        if let Some(element) = state.queue_element() {
            // Already completed, parse the result
            Poll::Ready(Output::try_from(element))
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
/// reaches the given time value.
pub fn sleep_until(executor: &Executor, time: Time) -> impl Future<Output = Result<()>> {
    new_async_operation(executor, move |queue_handle, context| {
        hel_check(unsafe {
            hel_sys::helSubmitAwaitClock(
                time.value(),
                queue_handle.handle(),
                context as usize,
                &mut 0, // We don't need the async operation ID
            )
        })
    })
}

/// Returns a future that will be completed after the given duration
/// has passed. This is equivalent to calling `sleep_until` with the
/// current time plus the given duration.
pub fn sleep_for(executor: &Executor, duration: Duration) -> impl Future<Output = Result<()>> {
    let time = Time::now().map(|time| time + duration);

    new_async_operation(executor, move |queue_handle, context| {
        hel_check(unsafe {
            hel_sys::helSubmitAwaitClock(
                time?.value(),
                queue_handle.handle(),
                context as usize,
                &mut 0, // We don't need the async operation ID
            )
        })
    })
}
