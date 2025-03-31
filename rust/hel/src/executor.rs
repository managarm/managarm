use std::cell::RefCell;
use std::future::Future;
use std::rc::Rc;
use std::task::{Context, Poll, Waker};

use crate::{Handle, OperationState, Queue, Result};

/// A single-threaded executor, takes care of completing submissions
/// and letting futures run to completion.
pub struct Executor {
    queue: RefCell<Queue>,
    queue_handle: Handle,
}

impl Executor {
    const QUEUE_RING_SHIFT: usize = 9;
    const QUEUE_CHUNK_COUNT: usize = 16;
    const QUEUE_CHUNK_SIZE: usize = 4096;

    /// Creates a new executor with a queue using default parameters.
    pub fn new() -> Result<Self> {
        let queue = Queue::new(
            Self::QUEUE_RING_SHIFT,
            Self::QUEUE_CHUNK_COUNT,
            Self::QUEUE_CHUNK_SIZE,
        )?;
        let queue_handle = queue.handle().clone_handle()?;

        Ok(Self {
            queue: RefCell::new(queue),
            queue_handle,
        })
    }

    /// Returns a reference to the queue's handle.
    /// This handle can be used to submit work to the queue.
    pub fn queue_handle(&self) -> &Handle {
        &self.queue_handle
    }

    /// Blocks the current thread until the given future is ready.
    pub fn block_on<F, R>(&self, future: F) -> Result<R>
    where
        F: Future<Output = Result<R>>,
    {
        // TODO: Not entirely sure how Rust's async stuff works, should this
        // maybe be a custom waker? Currently it uses a no-op waker and we just
        // poke the future "manually" when we get a submission but maybe it could
        // be implemented as a waker?

        let waker = Waker::noop();
        let mut cx = Context::from_waker(waker);
        let mut task = Box::pin(future);

        loop {
            match task.as_mut().poll(&mut cx) {
                Poll::Ready(res) => break res,
                Poll::Pending => {
                    let mut queue = self.queue.borrow_mut();
                    let element = queue.wait()?;

                    // This is safe to do because the operation state object is
                    // reference counter and we explicitly leak the reference to it
                    // when submitting the work. In case the future goes out of scope
                    // before the submission is completed, we will still have a
                    // valid reference we can use to complete the submission.
                    let state = unsafe { Rc::from_raw(element.context() as *const OperationState) };

                    // Complete the submission - this lets the future advance.
                    state.complete(element);
                }
            }
        }
    }
}
