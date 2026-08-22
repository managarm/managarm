use crate::sealed;

/// The set of observers that a memory space's accesses are ordered against.
///
/// libarch expresses this by having one `*_mem_ops` structure per domain; here the domain is a
/// parameter of the memory space, so that the barriers that an access needs are a property of the
/// space and not of the call site.
pub trait Domain: sealed::Sealed {
    fn before_load() {}
    fn after_load() {}
    fn before_store() {}
    fn after_store() {}
}

/// Ordering against devices, i.e. for MMIO.
#[derive(Clone, Copy)]
pub enum IoMemDomain {}

/// Ordering against other observers of main memory, i.e. for memory that is shared with a device.
#[derive(Clone, Copy)]
pub enum MainMemDomain {}

impl sealed::Sealed for IoMemDomain {}
impl sealed::Sealed for MainMemDomain {}

// x86 has TSO which is strong enough that a compiler barrier suffices to provide the ordering
// guarantees for both domains; no architectural fence is required.
#[cfg(target_arch = "x86_64")]
mod internal {
    use core::sync::atomic::{Ordering, compiler_fence};

    use super::{Domain, IoMemDomain, MainMemDomain};

    impl Domain for IoMemDomain {
        fn after_load() {
            compiler_fence(Ordering::SeqCst);
        }

        fn before_store() {
            compiler_fence(Ordering::SeqCst);
        }
    }

    impl Domain for MainMemDomain {
        fn after_load() {
            compiler_fence(Ordering::SeqCst);
        }

        fn before_store() {
            compiler_fence(Ordering::SeqCst);
        }
    }
}

#[cfg(target_arch = "aarch64")]
mod internal {
    use core::arch::asm;

    use super::{Domain, IoMemDomain, MainMemDomain};

    impl Domain for IoMemDomain {
        fn after_load() {
            unsafe { asm!("dmb oshld", options(nostack, preserves_flags)) };
        }

        fn before_store() {
            unsafe { asm!("dmb osh", options(nostack, preserves_flags)) };
        }
    }

    impl Domain for MainMemDomain {
        fn after_load() {
            unsafe { asm!("dmb ishld", options(nostack, preserves_flags)) };
        }

        fn before_store() {
            unsafe { asm!("dmb ish", options(nostack, preserves_flags)) };
        }
    }
}

#[cfg(target_arch = "riscv64")]
mod internal {
    use core::arch::asm;

    use super::{Domain, IoMemDomain, MainMemDomain};

    impl Domain for IoMemDomain {
        fn before_load() {
            unsafe { asm!("fence r, i", options(nostack, preserves_flags)) };
        }

        fn after_load() {
            unsafe { asm!("fence i, rw", options(nostack, preserves_flags)) };
        }

        fn before_store() {
            unsafe { asm!("fence rw, o", options(nostack, preserves_flags)) };
        }

        fn after_store() {
            unsafe { asm!("fence o, w", options(nostack, preserves_flags)) };
        }
    }

    impl Domain for MainMemDomain {
        fn after_load() {
            unsafe { asm!("fence r, rw", options(nostack, preserves_flags)) };
        }

        fn before_store() {
            unsafe { asm!("fence rw, w", options(nostack, preserves_flags)) };
        }
    }
}

#[cfg(not(any(
    target_arch = "x86_64",
    target_arch = "aarch64",
    target_arch = "riscv64"
)))]
compile_error!("Unsupported architecture");
