//! Cache maintenance for DMA to devices that are not cache-coherent.

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

    pub fn cache_clean_or_invalidate(addr: usize, size: usize) {
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
        // Even though a dmb is enough to order dc against memory accesses,
        // the ARMARM, section "Ordering and completion of data and instruction cache instructions"
        // requires a dsb to wait for the dc to complete from the perspective of other observers on the system bus.
        unsafe { asm!("dsb sy", options(nostack, preserves_flags)) };
    }

    // Clean and invalidate cache lines by VA to PoC.
    fn cache_clean_invalidate_poc(addr: usize, size: usize) {
        let dsz = dcache_line_size();
        let mut cur = addr & !(dsz - 1);
        while cur < addr + size {
            unsafe { asm!("dc civac, {}", in(reg) cur, options(nostack, preserves_flags)) };
            cur += dsz;
        }
        // Even though a dmb is enough to order dc against memory accesses,
        // the ARMARM, section "Ordering and completion of data and instruction cache instructions"
        // requires a dsb to wait for the dc to complete from the perspective of other observers on the system bus.
        unsafe { asm!("dsb sy", options(nostack, preserves_flags)) };
    }

    pub fn cache_writeback(addr: usize, size: usize) {
        cache_clean_poc(addr, size);
    }

    pub fn cache_clean_or_invalidate(addr: usize, size: usize) {
        cache_clean_poc(addr, size);
    }

    pub fn cache_invalidate(addr: usize, size: usize) {
        cache_clean_invalidate_poc(addr, size);
    }
}

#[cfg(target_arch = "riscv64")]
mod internal {
    use core::arch::asm;

    fn dcache_line_size() -> usize {
        // TODO: This information has to be obtained from the device tree.
        // 64 should be a safe guess for current CPUs (might result in some extra
        // flush/clean instructions at worst).
        64
    }

    // Clean cache lines.
    fn cache_clean(addr: usize, size: usize) {
        let dsz = dcache_line_size();
        let mut cur = addr & !(dsz - 1);
        while cur < addr + size {
            // Always allow Zicbom instructions, like libarch's gnu::target attribute.
            unsafe {
                asm!(
                    ".option push",
                    ".option arch, +zicbom",
                    "cbo.clean 0({})",
                    ".option pop",
                    in(reg) cur,
                    options(nostack, preserves_flags)
                )
            };
            cur += dsz;
        }
        unsafe { asm!("fence w, iorw", options(nostack, preserves_flags)) };
    }

    // Flush cache lines.
    fn cache_flush(addr: usize, size: usize) {
        let dsz = dcache_line_size();
        let mut cur = addr & !(dsz - 1);
        while cur < addr + size {
            unsafe {
                asm!(
                    ".option push",
                    ".option arch, +zicbom",
                    "cbo.flush 0({})",
                    ".option pop",
                    in(reg) cur,
                    options(nostack, preserves_flags)
                )
            };
            cur += dsz;
        }
        unsafe { asm!("fence w, iorw", options(nostack, preserves_flags)) };
    }

    pub fn cache_writeback(addr: usize, size: usize) {
        cache_clean(addr, size);
    }

    pub fn cache_clean_or_invalidate(addr: usize, size: usize) {
        cache_clean(addr, size);
    }

    pub fn cache_invalidate(addr: usize, size: usize) {
        cache_flush(addr, size);
    }
}

pub use internal::*;
