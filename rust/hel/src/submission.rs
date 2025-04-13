use std::{
    cell::Cell,
    pin::Pin,
    rc::Rc,
    task::{Context, LocalWaker, Poll},
};

use crate::{Executor, ExecutorInner, Handle, QueueElement, Result, Time, hel_check};

/// Common trait for completion result types.
pub trait Completion {
    /// Parses the result from the queue element.
    fn from_queue_element(element: QueueElement) -> Result<Self>
    where
        Self: Sized;
}

/// Common trait for all submissions.
pub unsafe trait Submission {
    /// Submits a submission to the queue.
    /// SAFETY: The implementation must ensure that no operation
    /// is submitted if the return value is an error.
    unsafe fn submit(&self, queue_handle: &Handle, context: usize) -> Result<()>;
}

/// A completion result that only consists of a status code.
pub struct SimpleResult;

impl Completion for SimpleResult {
    /// Parses a completion result from the queue element.
    fn from_queue_element(element: QueueElement) -> Result<Self> {
        let data = element.data();

        assert!(data.len() >= size_of::<hel_sys::HelSimpleResult>());

        // SAFETY: The data is guaranteed to contain enough bytes
        // to read a [`hel_sys::HelSimpleResult`]` and that it is
        // correctly aligned.
        let result = unsafe { data.as_ptr().cast::<hel_sys::HelSimpleResult>().read() };

        hel_check(result.error).map(|_| SimpleResult)
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

    fn poll(self: Pin<&mut Self>, cx: &mut Context<'_>) -> Poll<Self::Output> {
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

                // SAFETY: We expect the implementation to uphold the
                // safety contract of the [`Submission`] trait. The following
                // [`drop`] in the error case will drop the reference count
                // of the operation state object and release the memory.
                if let Err(err) = unsafe {
                    self.submission
                        .submit(self.state.executor.queue_handle(), context)
                } {
                    drop(unsafe { Rc::from_raw(context as *const OperationState) });

                    return Poll::Ready(Err(err));
                }
            }

            // Set the waker for this operation
            self.state.waker.set(Some(cx.local_waker().clone()));

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

unsafe impl Submission for AwaitClockSubmission {
    unsafe fn submit(&self, queue_handle: &Handle, context: usize) -> Result<()> {
        hel_check(unsafe {
            hel_sys::helSubmitAwaitClock(self.time.value(), queue_handle.handle(), context, &mut 0)
        })
    }
}

/// Creates a new `AwaitClockSubmission` for the time specified.
pub fn await_clock<'a>(
    executor: &Executor,
    time: Time,
) -> Operation<AwaitClockSubmission, SimpleResult> {
    let submission = AwaitClockSubmission { time };
    let state = Rc::new(OperationState::new(executor.clone_inner()));

    Operation {
        _marker: std::marker::PhantomData,
        state,
        submission,
    }
}
