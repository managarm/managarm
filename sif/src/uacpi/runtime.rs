//! The threads that run AML and uACPI's interrupt handlers.
//!
//! AML blocks the thread that runs it in Sleep(), Acquire() and Wait(), hence it only runs on
//! dedicated AML threads and never on an executor. Wrappers that may run AML take an [`Aml`]
//! token, which only exists on these threads.
//!
//! Each source of AML gets a thread of its own, such that AML that waits for an event from
//! another source cannot block that source: sif's own requests, uACPI's GPE methods and
//! uACPI's Notify() handlers each run on one thread here, and drivers can add their own
//! (e.g., for EC queries). uACPI's interrupt handlers never run AML; they run on yet another
//! thread that keeps serving interrupts while AML blocks.

use std::cell::Cell;
use std::marker::PhantomData;
use std::pin::Pin;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{OnceLock, mpsc};

use uacpi_sys::uacpi_work_type;

/// Proof that the current thread is an AML thread, i.e., that it may run AML.
#[derive(Clone, Copy)]
pub struct Aml {
    // Keeps the token on the thread that it belongs to.
    _marker: PhantomData<*const ()>,
}

/// Returns whether the current thread is an AML thread.
pub fn on_aml_thread() -> bool {
    Aml::current().is_some()
}

impl Aml {
    /// Returns a token if the current thread is an AML thread.
    pub(super) fn current() -> Option<Aml> {
        (!CURRENT_THREAD.get().is_null()).then_some(Aml {
            _marker: PhantomData,
        })
    }
}

thread_local! {
    /// The AML thread that runs on this thread, if any.
    static CURRENT_THREAD: Cell<*const AmlThread> = const { Cell::new(std::ptr::null()) };
    static ON_INTERRUPT_THREAD: Cell<bool> = const { Cell::new(false) };
}

type Work = Box<dyn FnOnce(Aml) + Send>;

/// A thread that runs AML, one piece of work after another.
pub struct AmlThread {
    sender: mpsc::Sender<Work>,
}

impl AmlThread {
    pub fn spawn(name: &str) -> &'static AmlThread {
        let (sender, receiver) = mpsc::channel::<Work>();
        let thread: &'static AmlThread = Box::leak(Box::new(AmlThread { sender }));
        std::thread::Builder::new()
            .name(name.into())
            .spawn(move || {
                CURRENT_THREAD.set(thread);
                let aml = Aml::current().expect("sif: AML thread has no token");
                while let Ok(work) = receiver.recv() {
                    work(aml);
                }
            })
            .expect("sif: failed to spawn an AML thread");
        thread
    }

    fn is_current(&self) -> bool {
        std::ptr::eq(CURRENT_THREAD.get(), self)
    }

    /// Runs f on the thread without waiting for it.
    pub fn post(&self, f: impl FnOnce(Aml) + Send + 'static) {
        self.sender
            .send(Box::new(f))
            .expect("sif: AML thread terminated");
    }

    /// Runs f on the thread; other tasks keep running while it does.
    pub async fn run<R: Send + 'static>(&self, f: impl FnOnce(Aml) -> R + Send + 'static) -> R {
        let (sender, receiver) = async_channel::bounded(1);
        self.post(move |aml| {
            let _ = sender.try_send(f(aml));
        });
        receiver.recv().await.expect("sif: AML thread terminated")
    }

    /// Runs f on the thread and blocks the calling thread until it returns.
    fn run_blocking<R: Send + 'static>(&self, f: impl FnOnce(Aml) -> R + Send + 'static) -> R {
        assert!(!self.is_current(), "sif: AML thread waits for itself");
        let (sender, receiver) = mpsc::sync_channel(1);
        self.post(move |aml| {
            let _ = sender.send(f(aml));
        });
        receiver.recv().expect("sif: AML thread terminated")
    }

    /// Waits until the work that was posted before has finished.
    fn flush(&self) {
        self.run_blocking(|_| ());
    }
}

/// The thread that runs AML on behalf of sif itself.
fn requests() -> &'static AmlThread {
    static THREAD: OnceLock<&'static AmlThread> = OnceLock::new();
    THREAD.get_or_init(|| AmlThread::spawn("acpi"))
}

/// Runs f on the AML thread that serves sif's own requests.
pub async fn run<R: Send + 'static>(f: impl FnOnce(Aml) -> R + Send + 'static) -> R {
    requests().run(f).await
}

static BLOCKING_ALLOWED: AtomicBool = AtomicBool::new(true);

/// Like [`run`], but blocks the calling thread, hence its executor, until f returns.
///
/// This is for synchronous code that needs AML, which only exists during initialization.
/// Once [`forbid_run_blocking`] has been called, this panics instead.
pub fn run_blocking<R: Send + 'static>(f: impl FnOnce(Aml) -> R + Send + 'static) -> R {
    assert!(
        BLOCKING_ALLOWED.load(Ordering::Relaxed),
        "sif: run_blocking() after initialization"
    );
    assert!(Aml::current().is_none(), "sif: AML thread blocks on AML");
    requests().run_blocking(f)
}

/// Ends initialization, i.e., makes later calls to [`run_blocking`] panic.
pub fn forbid_run_blocking() {
    BLOCKING_ALLOWED.store(false, Ordering::Relaxed);
}

/// The thread that runs GPE methods, i.e., _Lxx and _Exx.
fn gpe_thread() -> &'static AmlThread {
    static THREAD: OnceLock<&'static AmlThread> = OnceLock::new();
    THREAD.get_or_init(|| AmlThread::spawn("acpi-gpe"))
}

/// The thread that runs Notify() handlers.
fn notify_thread() -> &'static AmlThread {
    static THREAD: OnceLock<&'static AmlThread> = OnceLock::new();
    THREAD.get_or_init(|| AmlThread::spawn("acpi-notify"))
}

/// Implements uacpi_kernel_schedule_work().
pub fn schedule_work(work_type: uacpi_work_type, f: impl FnOnce(Aml) + Send + 'static) {
    match work_type {
        uacpi_sys::UACPI_WORK_GPE_EXECUTION => gpe_thread().post(f),
        _ => notify_thread().post(f),
    }
}

type InterruptTask = Box<dyn FnOnce() -> Pin<Box<dyn Future<Output = ()>>> + Send>;

/// The thread that runs uACPI's interrupt handlers.
fn interrupt_thread() -> &'static async_channel::Sender<InterruptTask> {
    static SENDER: OnceLock<async_channel::Sender<InterruptTask>> = OnceLock::new();
    SENDER.get_or_init(|| {
        let (sender, receiver) = async_channel::unbounded::<InterruptTask>();
        std::thread::Builder::new()
            .name("acpi-irq".into())
            .spawn(move || {
                ON_INTERRUPT_THREAD.set(true);
                hel::block_on(async move {
                    while let Ok(task) = receiver.recv().await {
                        hel::spawn(task());
                    }
                })
                .expect("sif: interrupt thread failed");
            })
            .expect("sif: failed to spawn the interrupt thread");
        sender
    })
}

/// Spawns the task that f returns onto the thread that runs uACPI's interrupt handlers.
///
/// The task must not run AML. It may block the interrupt thread only briefly.
pub fn spawn_interrupt_task<F, Fut>(f: F)
where
    F: FnOnce() -> Fut + Send + 'static,
    Fut: Future<Output = ()> + 'static,
{
    let task: InterruptTask = Box::new(move || Box::pin(f()));
    interrupt_thread()
        .try_send(task)
        .expect("sif: interrupt thread terminated");
}

/// Implements uacpi_kernel_wait_for_work_completion().
pub fn wait_for_work_completion() {
    assert!(
        !ON_INTERRUPT_THREAD.get(),
        "sif: interrupt thread waits for itself"
    );
    // uACPI's own work never waits here, only a handler that calls a waiting uACPI function.
    assert!(
        !gpe_thread().is_current() && !notify_thread().is_current(),
        "sif: a GPE or Notify() handler waits for the work that runs it"
    );

    // Interrupt handlers run synchronously, hence once a new task runs, earlier handlers are done.
    let (sender, receiver) = mpsc::sync_channel(1);
    spawn_interrupt_task(move || async move {
        let _ = sender.send(());
    });
    receiver.recv().expect("sif: interrupt thread terminated");

    // GPE methods can queue Notify() handlers, but not the other way around.
    gpe_thread().flush();
    notify_thread().flush();
}
