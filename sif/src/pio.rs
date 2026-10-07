//! thor enables I/O ports per thread, but sif accesses ports from several threads. Hence, a
//! window is granted once and accessors enable it on the current thread before the first access.

use arch::PioSpace;
use std::ops::Range;

#[cfg(target_arch = "x86_64")]
const EXPECT_LOCK: &str = "sif: PIO window ID mutex was poisoned";

/// Window IDs are dense, such that ENABLED only grows with the number of live windows.
#[cfg(target_arch = "x86_64")]
struct Ids {
    /// The generation of each ID, which changes whenever the ID is released.
    generations: Vec<u64>,
    free: Vec<usize>,
}

#[cfg(target_arch = "x86_64")]
impl Ids {
    fn allocate(&mut self) -> (usize, u64) {
        if let Some(id) = self.free.pop() {
            return (id, self.generations[id]);
        }
        self.generations.push(1);
        (self.generations.len() - 1, 1)
    }

    fn release(&mut self, id: usize) {
        self.generations[id] += 1;
        self.free.push(id);
    }
}

#[cfg(target_arch = "x86_64")]
static IDS: std::sync::Mutex<Ids> = std::sync::Mutex::new(Ids {
    generations: Vec::new(),
    free: Vec::new(),
});

#[cfg(target_arch = "x86_64")]
std::thread_local! {
    /// For each window ID, the generation that is enabled on the current thread, or 0.
    ///
    /// Entries of released IDs are stale but harmless: the next window gets a newer generation.
    static ENABLED: std::cell::RefCell<Vec<u64>> = const { std::cell::RefCell::new(Vec::new()) };
}

/// A range of I/O ports that sif has been granted access to.
pub struct PioWindow {
    ports: Range<usize>,
    #[cfg(target_arch = "x86_64")]
    id: usize,
    #[cfg(target_arch = "x86_64")]
    generation: u64,
    #[cfg(target_arch = "x86_64")]
    io: hel::Handle,
}

impl PioWindow {
    pub fn new(ports: Range<usize>) -> hel::Result<Self> {
        if ports.end > 1 << 16 {
            return Err(hel::Error::IllegalArgs);
        }

        #[cfg(target_arch = "x86_64")]
        let io = {
            let list: Vec<usize> = ports.clone().collect();
            hel::access_io(managarm::svrctl::hardware_access_handle(), &list)?
        };
        #[cfg(target_arch = "x86_64")]
        let (id, generation) = IDS.lock().expect(EXPECT_LOCK).allocate();
        Ok(Self {
            ports,
            #[cfg(target_arch = "x86_64")]
            id,
            #[cfg(target_arch = "x86_64")]
            generation,
            #[cfg(target_arch = "x86_64")]
            io,
        })
    }

    pub fn len(&self) -> usize {
        self.ports.len()
    }

    /// Enables the window on the current thread unless that happened before.
    ///
    /// The returned space must only be accessed from the current thread.
    pub fn enable(&self) -> hel::Result<PioSpace> {
        #[cfg(target_arch = "x86_64")]
        ENABLED.with_borrow_mut(|enabled| {
            if enabled.len() <= self.id {
                enabled.resize(self.id + 1, 0);
            }
            if enabled[self.id] != self.generation {
                hel::enable_io(&self.io)?;
                enabled[self.id] = self.generation;
            }
            Ok::<_, hel::Error>(())
        })?;
        Ok(unsafe { PioSpace::new(self.ports.start as u16) })
    }
}

#[cfg(target_arch = "x86_64")]
impl Drop for PioWindow {
    fn drop(&mut self) {
        IDS.lock().expect(EXPECT_LOCK).release(self.id);
    }
}
