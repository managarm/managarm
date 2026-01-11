use std::{
    cell::{Cell, RefCell},
    collections::VecDeque,
    future::Future,
    pin::Pin,
    rc::{Rc, Weak},
    task::{ContextBuilder, LocalWake, LocalWaker, Waker},
};

use crate::{handle::Handle, queue::Queue, result::Result, submission::OperationState};

type BoxedFuture = Box<dyn Future<Output = ()>>;
type RunQueue = VecDeque<Rc<Task>>;

struct Task {
    future: RefCell<Pin<BoxedFuture>>,
    executor: Weak<ExecutorInner>,
}

impl LocalWake for Task {
    fn wake(self: Rc<Self>) {
        match self.executor.upgrade() {
            Some(executor) => {
                let mut run_queue = executor.run_queue.borrow_mut();
                run_queue.push_back(self.clone());
            }
            None => {
                // The executor has been dropped, we can't do anything
                // with this task anymore.
            }
        }
    }
}

pub(crate) struct ExecutorInner {
    queue: RefCell<Queue>,
    run_queue: RefCell<RunQueue>,
    queue_handle: Handle,
}

impl ExecutorInner {
    pub fn queue_handle(&self) -> &Handle {
        &self.queue_handle
    }
}

/// A single-threaded executor, takes care of completing submissions
/// and letting futures run to completion.
#[derive(Clone)]
pub struct Executor {
    inner: Rc<ExecutorInner>,
}

impl Executor {
    const QUEUE_CHUNK_COUNT: usize = 16;
    const QUEUE_CHUNK_SIZE: usize = 4096;

    /// Creates a new executor with a queue using default parameters.
    pub fn new() -> Result<Self> {
        let queue = Queue::new(Self::QUEUE_CHUNK_COUNT, Self::QUEUE_CHUNK_SIZE)?;

        let queue_handle = queue.handle().clone_handle()?;

        Ok(Self {
            inner: Rc::new(ExecutorInner {
                queue: RefCell::new(queue),
                run_queue: RefCell::new(VecDeque::new()),
                queue_handle,
            }),
        })
    }

    /// Returns a reference to the queue's handle.
    /// This handle can be used to submit work to the queue.
    pub fn queue_handle(&self) -> &Handle {
        self.inner.queue_handle()
    }

    /// Spawns a new task to the executor. This task will be executed
    /// when the executor is run. The task must be a future that
    /// returns a value of type `()`.
    pub fn spawn<F>(&self, future: F)
    where
        F: Future<Output = ()> + 'static,
    {
        self.inner.run_queue.borrow_mut().push_back(Rc::new(Task {
            future: RefCell::new(Box::pin(future)),
            executor: Rc::downgrade(&self.inner),
        }));
    }

    /// Runs the executor once, executing all tasks in the run queue and
    /// returning true if any task was completed.
    pub fn run_once(&self) -> bool {
        while let Some(task) = {
            let mut queue = self.inner.run_queue.borrow_mut();
            queue.pop_front()
        } {
            let waker = LocalWaker::from(task.clone());
            let mut cx = ContextBuilder::from_waker(Waker::noop())
                .local_waker(&waker)
                .build();

            if task.future.borrow_mut().as_mut().poll(&mut cx).is_ready() {
                return true;
            }
        }

        false
    }

    /// Waits for a submission to complete. This will block the current
    /// thread until a submission is completed.
    pub fn wait(&self) -> Result<()> {
        // No tasks in the run queue, wait for a submission to wake us up
        let mut queue = self.inner.queue.borrow_mut();
        let element = queue.wait()?;

        // SAFETY: We only ever enqueue operation state objects onto the
        // queue, and we leak a reference in the process so that we can
        // soundly subtract it here.
        let state = unsafe { Rc::from_raw(element.context() as *const OperationState) };

        // Complete the submission - this lets the future advance.
        state.complete(element);

        Ok(())
    }

    /// Blocks the current thread until the given future is ready.
    pub fn block_on<F, R: 'static>(&self, future: F) -> Result<R>
    where
        F: Future<Output = R> + 'static,
    {
        let result = Rc::new(Cell::new(None));
        let result_clone = result.clone();

        self.spawn(async move {
            result_clone.set(Some(future.await));
        });

        loop {
            if !self.run_once() {
                // No result yet, wait for a submission to wake us up
                self.wait()?;
            } else if let Some(result) = result.take() {
                return Ok(result);
            }
        }
    }

    /// Creates a new reference to the executor's inner state.
    pub(crate) fn clone_inner(&self) -> Rc<ExecutorInner> {
        self.inner.clone()
    }
}

/// Spawns a new task.
pub fn spawn<F>(future: F)
where
    F: Future<Output = ()> + 'static,
{
    EXECUTOR.with(|executor| executor.borrow().spawn(future));
}

/// Blocks the current thread until the given future is ready.
pub fn block_on<F, R: 'static>(future: F) -> Result<R>
where
    F: Future<Output = R> + 'static,
{
    EXECUTOR.with(|executor| executor.borrow().block_on(future))
}

/// Temporarily replaces the default per-thread executor with the given one.
/// The returned value ensures that the executor is restored to its previous state
/// when dropped.
pub fn enter_executor(new_executor: Executor) -> ExecutorGuard {
    let previous_executor = EXECUTOR.with(|executor| executor.replace(new_executor));

    ExecutorGuard { previous_executor }
}

/// Returns a new reference to the current executor.
pub fn current_executor() -> Executor {
    EXECUTOR.with(|executor| executor.borrow().clone())
}

thread_local! {
    /// The current executor for the thread.
    pub(crate) static EXECUTOR: RefCell<Executor>
        = RefCell::new(Executor::new().expect("Failed to create executor"));
}

pub struct ExecutorGuard {
    previous_executor: Executor,
}

impl Drop for ExecutorGuard {
    fn drop(&mut self) {
        EXECUTOR.with(|executor| executor.replace(self.previous_executor.clone()));
    }
}
