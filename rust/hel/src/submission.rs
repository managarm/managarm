use std::{
    cell::Cell,
    pin::Pin,
    rc::Rc,
    task::{Context, Poll},
};

use crate::{Executor, Handle, QueueElement, Result, hel_check};

/// Common trait for completion result types.
pub trait ResultType {
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

impl ResultType for SimpleResult {
    /// Parses a completion result from the queue element.
    fn from_queue_element(element: QueueElement) -> Result<Self> {
        let data = element.data();

        assert!(data.len() == size_of::<hel_sys::HelSimpleResult>());

        let result = unsafe { data.as_ptr().cast::<hel_sys::HelSimpleResult>().read() };

        hel_check(result.error).map(|_| SimpleResult)
    }
}

/// Raw submission object. This is used to store the submission and completion
/// status alond with any other data that is needed to submit and complete work.
pub(crate) struct RawSubmission<'a> {
    is_submitted: Cell<bool>,
    element: Cell<Option<QueueElement<'a>>>,
    queue_handle: &'a Handle,
}

impl<'a> RawSubmission<'a> {
    /// Creates a new raw submission object.
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

/// Concrete submission object. This is a wrapper around the raw submission
/// object which is used by the executor to complete submissions and
/// the submission itself which is used to submit the work to the queue.
pub struct ConcreteSubmission<'a, S: Submission, R: ResultType> {
    _marker: std::marker::PhantomData<R>,
    raw: Rc<RawSubmission<'a>>,
    submission: S,
}

impl<S: Submission, R: ResultType> Future for ConcreteSubmission<'_, S, R> {
    type Output = Result<R>;

    fn poll(self: Pin<&mut Self>, _cx: &mut Context<'_>) -> Poll<Self::Output> {
        if let Some(element) = self.raw.queue_element() {
            // Already completed, parse the result
            Poll::Ready(R::from_queue_element(element))
        } else {
            if !self.raw.is_submitted() {
                // Not submitted yet, submit it

                // Bump the reference count of the raw submission object
                // and leak the created reference so that it doesn't get dropped
                // in case the future goes out of scope - it will be dropped
                // once the submission is completed.

                let raw_cloned = self.raw.clone();
                let context = Rc::into_raw(raw_cloned) as usize;

                if let Err(err) = self.submission.submit(self.raw.queue_handle, context) {
                    return Poll::Ready(Err(err));
                }
            }

            // Now we can wait for it to finish
            Poll::Pending
        }
    }
}

/// Submission for a clock event.
/// This is used to wait for the clock to reach a certain value.
/// The clock value is specified in nanoseconds since boot.
pub struct AwaitClockSubmission {
    counter: u64,
}

impl Submission for AwaitClockSubmission {
    fn submit(&self, queue_handle: &Handle, context: usize) -> Result<()> {
        hel_check(unsafe {
            hel_sys::helSubmitAwaitClock(self.counter, queue_handle.handle(), context, &mut 0)
        })
    }
}

/// Creates a new `AwaitClockSubmission` for the given clock value.
/// This is a future that can be awaited on and will only be completed
/// when the clock reaches the specified value.
pub fn await_clock(
    executor: &Executor,
    clock: u64,
) -> ConcreteSubmission<AwaitClockSubmission, SimpleResult> {
    let submission = AwaitClockSubmission { counter: clock };
    let raw = Rc::new(RawSubmission::new(executor.queue_handle()));

    ConcreteSubmission {
        _marker: std::marker::PhantomData,
        raw,
        submission,
    }
}
