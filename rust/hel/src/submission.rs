use std::{
    cell::Cell,
    pin::Pin,
    rc::Rc,
    task::{Context, Poll},
};

use crate::{Executor, Handle, QueueElement, Result, Time, hel_check};

/// Common trait for completion result types.
pub trait Completion {
    /// Parses the result from the queue element.
    fn from_queue_element(element: QueueElement) -> Result<Self>
    where
        Self: Sized;
}

/// Common trait for all submissions.
pub trait Submission {
    /// Submits a submission to the queue.
    fn submit(&self, queue_handle: &Handle, context: usize) -> Result<()>;
}

/// A completion result that only consists of a status code.
pub struct SimpleResult;

impl Completion for SimpleResult {
    /// Parses a completion result from the queue element.
    fn from_queue_element(element: QueueElement) -> Result<Self> {
        let data = element.data();

        assert!(data.len() == size_of::<hel_sys::HelSimpleResult>());

        let result = unsafe { data.as_ptr().cast::<hel_sys::HelSimpleResult>().read() };

        hel_check(result.error).map(|_| SimpleResult)
    }
}

/// Operation state object. This is used to store the submission and completion
/// status alond with any other data that is needed to submit and complete work.
pub(crate) struct OperationState<'a> {
    is_submitted: Cell<bool>,
    element: Cell<Option<QueueElement<'a>>>,
    queue_handle: &'a Handle,
}

impl<'a> OperationState<'a> {
    /// Creates a new operation state object.
    fn new(queue_handle: &'a Handle) -> Self {
        Self {
            is_submitted: Cell::new(false),
            element: Cell::new(None),
            queue_handle,
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
    }
}

/// Operation object. This is a wrapper around the operation state
/// object which is used by the executor to complete submissions and
/// the submission itself which is used to submit the work to the queue.
pub struct Operation<'a, S: Submission, R: Completion> {
    _marker: std::marker::PhantomData<R>,
    state: Rc<OperationState<'a>>,
    submission: S,
}

impl<S: Submission, R: Completion> Future for Operation<'_, S, R> {
    type Output = Result<R>;

    fn poll(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<Self::Output> {
        if let Some(element) = self.state.queue_element() {
            // Already completed, parse the result
            Poll::Ready(R::from_queue_element(element))
        } else {
            if !self.state.is_submitted() {
                // Not submitted yet, submit it

                // Bump the reference count of the operation state object
                // and leak the created reference so that it doesn't get dropped
                // in case the future goes out of scope - it will be dropped
                // once the submission is completed.

                let state_cloned = self.state.clone();
                let context = Rc::into_raw(state_cloned) as usize;

                if let Err(err) = self.submission.submit(self.state.queue_handle, context) {
                    return Poll::Ready(Err(err));
                }
            }

            // Now we can wait for it to finish
            Poll::Pending
        }
    }
}

/// Submission for a clock event. This is used to wait for the clock
/// to reach a certain value.
pub struct AwaitClockSubmission {
    time: Time,
}

impl Submission for AwaitClockSubmission {
    fn submit(&self, queue_handle: &Handle, context: usize) -> Result<()> {
        hel_check(unsafe {
            hel_sys::helSubmitAwaitClock(self.time.value(), queue_handle.handle(), context, &mut 0)
        })
    }
}

/// Creates a new `AwaitClockSubmission` for the time specified.
pub fn await_clock(
    executor: &Executor,
    time: Time,
) -> Operation<AwaitClockSubmission, SimpleResult> {
    let submission = AwaitClockSubmission { time };
    let state = Rc::new(OperationState::new(executor.queue_handle()));

    Operation {
        _marker: std::marker::PhantomData,
        state,
        submission,
    }
}
