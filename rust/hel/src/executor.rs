use alloc::{boxed::Box, collections::VecDeque, rc::Rc};
#[cfg(feature = "std")]
use core::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use core::{
    cell::RefCell,
    future::Future,
    pin::Pin,
    task::{Context, Poll, Waker},
};
#[cfg(feature = "std")]
use std::sync::Mutex;

use async_task::{Runnable, Task};

#[cfg(feature = "std")]
use crate::result::hel_check;
use crate::{handle::Handle, queue::Queue, result::Result, submission::OperationState};

#[cfg(feature = "std")]
const EXPECT_LOCK: &str = "hel: executor lock poisoned";

/// Returns a value that identifies the current thread and is never reused.
#[cfg(feature = "std")]
fn thread_token() -> usize {
    static NEXT_TOKEN: AtomicUsize = AtomicUsize::new(0);
    std::thread_local!(static TOKEN: usize = NEXT_TOKEN.fetch_add(1, Ordering::Relaxed));
    TOKEN.with(|token| *token)
}

/// State that is shared between an executor and the schedule functions of its tasks.
/// It is leaked when the executor is created: executors are not destructed, and
/// wakers may outlive the thread that owns the executor.
struct Shared {
    /// Token of the thread that owns the executor.
    #[cfg(feature = "std")]
    owner: usize,
    /// Tasks that are ready to run. Only accessed on the owner thread.
    run_queue: RefCell<VecDeque<Runnable>>,
    /// Tasks woken up on other threads, drained by the owner thread.
    #[cfg(feature = "std")]
    remote: Mutex<VecDeque<Runnable>>,
    /// Set when tasks are added to `remote`, read by the owner without taking the lock.
    #[cfg(feature = "std")]
    remote_posted: AtomicBool,
    /// Handle of the executor's queue, alerted when `remote` becomes non-empty.
    queue_handle: Handle,
}

// SAFETY: `run_queue` is only accessed on the owner thread: with `std`, schedule()
// checks the thread before touching it, and without `std` the process is single-threaded.
unsafe impl Sync for Shared {}

impl Shared {
    /// Returns whether the current thread owns the executor.
    #[cfg(feature = "std")]
    fn is_owner(&self) -> bool {
        thread_token() == self.owner
    }

    /// Returns whether the current thread owns the executor.
    #[cfg(not(feature = "std"))]
    fn is_owner(&self) -> bool {
        true
    }

    /// Schedules a task that has been woken up.
    fn schedule(&self, runnable: Runnable) {
        if !self.is_owner() {
            self.schedule_remote(runnable);
            return;
        }

        self.run_queue.borrow_mut().push_back(runnable);
    }

    /// Schedules a task that has been woken up on a thread other than the owner.
    #[cfg(feature = "std")]
    fn schedule_remote(&self, runnable: Runnable) {
        let mut remote = self.remote.lock().expect(EXPECT_LOCK);
        // As in libasync's run_queue, only the transition to non-empty needs an alert:
        // tasks added later are drained together with the first one.
        let needs_alert = remote.is_empty();
        remote.push_back(runnable);
        // Relaxed is enough: the owner reads this after an acquiring RMW on userNotify
        // that synchronizes with the alert, or after taking the lock.
        self.remote_posted.store(true, Ordering::Relaxed);
        drop(remote);

        if needs_alert {
            hel_check(unsafe { hel_sys::helAlertQueue(self.queue_handle.handle()) })
                .expect("Failed to alert queue");
        }
    }

    /// Schedules a task that has been woken up on a thread other than the owner.
    #[cfg(not(feature = "std"))]
    fn schedule_remote(&self, _runnable: Runnable) {
        unreachable!()
    }

    /// Returns whether tasks have been woken up on other threads since the last drain.
    #[cfg(feature = "std")]
    fn remote_posted(&self) -> bool {
        self.remote_posted.load(Ordering::Relaxed)
    }

    /// Returns whether tasks have been woken up on other threads since the last drain.
    #[cfg(not(feature = "std"))]
    fn remote_posted(&self) -> bool {
        false
    }

    /// Moves tasks that have been woken up on other threads to the run queue.
    #[cfg(feature = "std")]
    fn drain_remote(&self) {
        if !self.remote_posted() {
            return;
        }

        let runnables = {
            let mut remote = self.remote.lock().expect(EXPECT_LOCK);
            // Reset under the lock, so that the next task finds the queue empty and alerts.
            self.remote_posted.store(false, Ordering::Relaxed);
            core::mem::take(&mut *remote)
        };
        self.run_queue.borrow_mut().extend(runnables);
    }

    /// Moves tasks that have been woken up on other threads to the run queue.
    #[cfg(not(feature = "std"))]
    fn drain_remote(&self) {}

    /// Removes the next task from the run queue.
    fn pop(&self) -> Option<Runnable> {
        self.run_queue.borrow_mut().pop_front()
    }
}

pub(crate) struct ExecutorInner {
    queue: RefCell<Queue>,
    shared: &'static Shared,
}

impl ExecutorInner {
    pub fn queue_handle(&self) -> &Handle {
        &self.shared.queue_handle
    }

    /// Pushes an element to the submission queue using a gather list.
    pub fn push_sq(
        &self,
        opcode: u32,
        context: usize,
        segments: &[&[u8]],
    ) -> crate::result::Result<()> {
        self.queue.borrow_mut().push_sq(opcode, context, segments)
    }

    /// Creates a task that runs the given future and schedules it.
    fn spawn_task<F>(&self, future: F) -> Task<F::Output>
    where
        F: Future + 'static,
        F::Output: 'static,
    {
        let shared = self.shared;
        let schedule = move |runnable: Runnable| shared.schedule(runnable);
        assert_send_sync(&schedule);

        // SAFETY: The future and its output are 'static, and so is the schedule
        // function, which is also Send + Sync. The Runnable is only run or dropped
        // on the owner thread: Executor is !Send, so spawn_task() and run_once() run
        // there, and schedule() on other threads only parks it in remote to be drained.
        let (runnable, task) = unsafe { async_task::spawn_unchecked(future, schedule) };
        runnable.schedule();
        task
    }
}

/// Checks at compile time that the given value can be shared between threads.
fn assert_send_sync<T: Send + Sync>(_: &T) {}

/// A single-threaded executor, takes care of completing submissions
/// and letting futures run to completion.
#[derive(Clone)]
pub struct Executor {
    inner: Rc<ExecutorInner>,
}

impl Executor {
    const QUEUE_CHUNK_COUNT: usize = 16;
    const QUEUE_CHUNK_SIZE: usize = 4096;
    const QUEUE_SQ_CHUNK_COUNT: usize = 2;

    /// Creates a new executor with a queue using default parameters.
    pub fn new() -> Result<Self> {
        let queue = Queue::new_with_sq(
            Self::QUEUE_CHUNK_COUNT,
            Self::QUEUE_CHUNK_SIZE,
            Self::QUEUE_SQ_CHUNK_COUNT,
        )?;

        let queue_handle = queue.handle().clone_handle()?;

        Ok(Self {
            inner: Rc::new(ExecutorInner {
                queue: RefCell::new(queue),
                shared: Box::leak(Box::new(Shared {
                    #[cfg(feature = "std")]
                    owner: thread_token(),
                    run_queue: RefCell::new(VecDeque::new()),
                    #[cfg(feature = "std")]
                    remote: Mutex::new(VecDeque::new()),
                    #[cfg(feature = "std")]
                    remote_posted: AtomicBool::new(false),
                    queue_handle,
                })),
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
        self.inner.spawn_task(future).detach();
    }

    /// Runs the executor once, executing all tasks in the run queue and
    /// returning true if any task was run.
    pub fn run_once(&self) -> bool {
        let mut ran = false;

        self.inner.shared.drain_remote();
        while let Some(runnable) = self.inner.shared.pop() {
            runnable.run();
            ran = true;
        }

        ran
    }

    /// Waits for a submission to complete. This will block the current
    /// thread until a submission is completed or a task is woken up
    /// on another thread.
    fn wait(&self) -> Result<()> {
        // No tasks in the run queue, wait for a submission to wake us up
        let mut queue = self.inner.queue.borrow_mut();
        let Some(element) = queue.wait(|| self.inner.shared.remote_posted())? else {
            // Tasks have been woken up on other threads, run_once() picks them up.
            return Ok(());
        };

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
        let mut task = self.inner.spawn_task(future);

        loop {
            self.run_once();

            if task.is_finished() {
                // The task has completed, so polling it returns its output right away.
                let mut cx = Context::from_waker(Waker::noop());
                match Pin::new(&mut task).poll(&mut cx) {
                    Poll::Ready(result) => return Ok(result),
                    Poll::Pending => unreachable!(),
                }
            }

            // No result yet, wait for a submission to wake us up
            self.wait()?;
        }
    }

    /// Creates a new reference to the executor's inner state.
    pub(crate) fn clone_inner(&self) -> Rc<ExecutorInner> {
        self.inner.clone()
    }
}

/// Spawns a new task.
#[cfg(feature = "std")]
pub fn spawn<F>(future: F)
where
    F: Future<Output = ()> + 'static,
{
    EXECUTOR.with(|executor| executor.borrow().spawn(future));
}

/// Blocks the current thread until the given future is ready.
#[cfg(feature = "std")]
pub fn block_on<F, R: 'static>(future: F) -> Result<R>
where
    F: Future<Output = R> + 'static,
{
    EXECUTOR.with(|executor| executor.borrow().block_on(future))
}

/// Temporarily replaces the default per-thread executor with the given one.
/// The returned value ensures that the executor is restored to its previous state
/// when dropped.
#[cfg(feature = "std")]
pub fn enter_executor(new_executor: Executor) -> ExecutorGuard {
    let previous_executor = EXECUTOR.with(|executor| executor.replace(new_executor));

    ExecutorGuard { previous_executor }
}

/// Returns a new reference to the current executor.
#[cfg(feature = "std")]
pub fn current_executor() -> Executor {
    EXECUTOR.with(|executor| executor.borrow().clone())
}

#[cfg(feature = "std")]
std::thread_local! {
    /// The current executor for the thread.
    pub(crate) static EXECUTOR: RefCell<Executor>
        = RefCell::new(Executor::new().expect("Failed to create executor"));
}

#[cfg(feature = "std")]
pub struct ExecutorGuard {
    previous_executor: Executor,
}

#[cfg(feature = "std")]
impl Drop for ExecutorGuard {
    fn drop(&mut self) {
        EXECUTOR.with(|executor| executor.replace(self.previous_executor.clone()));
    }
}
