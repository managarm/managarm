//! Cache maintenance operations, ported from libarch.

#[cfg(target_arch = "x86_64")]
mod internal {
    use core::arch::asm;

    fn dcache_line_size() -> usize {
        // TODO: This information has to be obtained from CPUID.
        // 64 should be a safe guess for current CPUs (might result in some extra
        // clflush instructions at worst).
        64
    }

    fn cache_clflush(addr: usize, size: usize) {
        let dsz = dcache_line_size();
        let mut cur = addr & !(dsz - 1);
        while cur < addr + size {
            unsafe { asm!("clflush [{}]", in(reg) cur, options(nostack, preserves_flags)) };
            cur += dsz;
        }
    }

    pub fn cache_writeback(addr: usize, size: usize) {
        cache_clflush(addr, size);
    }

    pub fn cache_invalidate(addr: usize, size: usize) {
        cache_clflush(addr, size);
    }
}

#[cfg(target_arch = "aarch64")]
mod internal {
    use core::arch::asm;

    fn dcache_line_size() -> usize {
        let ctr: u64;
        unsafe { asm!("mrs {}, ctr_el0", out(reg) ctr, options(nomem, nostack, preserves_flags)) };

        4 << ((ctr >> 16) & 0b1111)
    }

    // Clean cache lines by VA to PoC.
    fn cache_clean_poc(addr: usize, size: usize) {
        let dsz = dcache_line_size();
        let mut cur = addr & !(dsz - 1);
        while cur < addr + size {
            unsafe { asm!("dc cvac, {}", in(reg) cur, options(nostack, preserves_flags)) };
            cur += dsz;
        }
        unsafe { asm!("dmb sy", options(nostack, preserves_flags)) };
    }

    // Clean and invalidate cache lines by VA to PoC.
    fn cache_clean_invalidate_poc(addr: usize, size: usize) {
        let dsz = dcache_line_size();
        let mut cur = addr & !(dsz - 1);
        while cur < addr + size {
            unsafe { asm!("dc civac, {}", in(reg) cur, options(nostack, preserves_flags)) };
            cur += dsz;
        }
        unsafe { asm!("dmb sy", options(nostack, preserves_flags)) };
    }

    pub fn cache_writeback(addr: usize, size: usize) {
        cache_clean_poc(addr, size);
    }

    pub fn cache_invalidate(addr: usize, size: usize) {
        cache_clean_invalidate_poc(addr, size);
    }
}

// Cache maintenance from user space is not implemented on other architectures; no device that
// we drive needs it there.
#[cfg(not(any(target_arch = "x86_64", target_arch = "aarch64")))]
mod internal {
    pub fn cache_writeback(_addr: usize, _size: usize) {
        unimplemented!()
    }

    pub fn cache_invalidate(_addr: usize, _size: usize) {
        unimplemented!()
    }
}

pub use internal::*;
