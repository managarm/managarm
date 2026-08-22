use core::marker::PhantomData;

use crate::bits::BitConvertible;
use crate::domain::{Domain, IoMemDomain, MainMemDomain};
use crate::register::Register;
use crate::sealed;

/// A type that a single memory transaction can carry.
///
/// The accesses are relaxed, i.e. they carry no ordering guarantees; [`BaseMemSpace`] combines
/// them with the barriers of its domain, mirroring how libarch builds its `*_mem_ops` on top of
/// `relaxed_mem_ops`. They are inline assembly and not volatile pointer accesses to guarantee
/// that the compiler emits neither an unexpected instruction nor a torn access.
pub trait MemAccess: sealed::Sealed {
    /// # Safety
    ///
    /// `p` has to point to a naturally aligned `Self` that stays mapped for the access.
    unsafe fn load_relaxed(p: *const Self) -> Self;

    /// # Safety
    ///
    /// `p` has to point to a naturally aligned `Self` that stays mapped for the access.
    unsafe fn store_relaxed(p: *mut Self, value: Self);
}

#[cfg(target_arch = "x86_64")]
mod internal {
    use core::arch::asm;

    use super::MemAccess;

    impl MemAccess for u8 {
        unsafe fn load_relaxed(p: *const u8) -> u8 {
            let value: u8;
            unsafe {
                asm!(
                    "mov {value}, byte ptr [{ptr}]",
                    value = out(reg_byte) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u8, value: u8) {
            unsafe {
                asm!(
                    "mov byte ptr [{ptr}], {value}",
                    ptr = in(reg) p,
                    value = in(reg_byte) value,
                    options(nostack, preserves_flags),
                )
            };
        }
    }

    impl MemAccess for u16 {
        unsafe fn load_relaxed(p: *const u16) -> u16 {
            let value: u16;
            unsafe {
                asm!(
                    "mov {value:x}, word ptr [{ptr}]",
                    value = out(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u16, value: u16) {
            unsafe {
                asm!(
                    "mov word ptr [{ptr}], {value:x}",
                    ptr = in(reg) p,
                    value = in(reg) value,
                    options(nostack, preserves_flags),
                )
            };
        }
    }

    impl MemAccess for u32 {
        unsafe fn load_relaxed(p: *const u32) -> u32 {
            let value: u32;
            unsafe {
                asm!(
                    "mov {value:e}, dword ptr [{ptr}]",
                    value = out(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u32, value: u32) {
            unsafe {
                asm!(
                    "mov dword ptr [{ptr}], {value:e}",
                    ptr = in(reg) p,
                    value = in(reg) value,
                    options(nostack, preserves_flags),
                )
            };
        }
    }

    impl MemAccess for u64 {
        unsafe fn load_relaxed(p: *const u64) -> u64 {
            let value: u64;
            unsafe {
                asm!(
                    "mov {value:r}, qword ptr [{ptr}]",
                    value = out(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u64, value: u64) {
            unsafe {
                asm!(
                    "mov qword ptr [{ptr}], {value:r}",
                    ptr = in(reg) p,
                    value = in(reg) value,
                    options(nostack, preserves_flags),
                )
            };
        }
    }
}

#[cfg(target_arch = "aarch64")]
mod internal {
    use core::arch::asm;

    use super::MemAccess;

    impl MemAccess for u8 {
        unsafe fn load_relaxed(p: *const u8) -> u8 {
            let value: u8;
            unsafe {
                asm!(
                    "ldrb {value:w}, [{ptr}]",
                    value = out(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u8, value: u8) {
            unsafe {
                asm!(
                    "strb {value:w}, [{ptr}]",
                    value = in(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
        }
    }

    impl MemAccess for u16 {
        unsafe fn load_relaxed(p: *const u16) -> u16 {
            let value: u16;
            unsafe {
                asm!(
                    "ldrh {value:w}, [{ptr}]",
                    value = out(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u16, value: u16) {
            unsafe {
                asm!(
                    "strh {value:w}, [{ptr}]",
                    value = in(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
        }
    }

    impl MemAccess for u32 {
        unsafe fn load_relaxed(p: *const u32) -> u32 {
            let value: u32;
            unsafe {
                asm!(
                    "ldr {value:w}, [{ptr}]",
                    value = out(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u32, value: u32) {
            unsafe {
                asm!(
                    "str {value:w}, [{ptr}]",
                    value = in(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
        }
    }

    impl MemAccess for u64 {
        unsafe fn load_relaxed(p: *const u64) -> u64 {
            let value: u64;
            unsafe {
                asm!(
                    "ldr {value:x}, [{ptr}]",
                    value = out(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u64, value: u64) {
            unsafe {
                asm!(
                    "str {value:x}, [{ptr}]",
                    value = in(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
        }
    }
}

#[cfg(target_arch = "riscv64")]
mod internal {
    use core::arch::asm;

    use super::MemAccess;

    impl MemAccess for u8 {
        unsafe fn load_relaxed(p: *const u8) -> u8 {
            let value: u8;
            unsafe {
                asm!(
                    "lbu {value}, 0({ptr})",
                    value = out(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u8, value: u8) {
            unsafe {
                asm!(
                    "sb {value}, 0({ptr})",
                    value = in(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
        }
    }

    impl MemAccess for u16 {
        unsafe fn load_relaxed(p: *const u16) -> u16 {
            let value: u16;
            unsafe {
                asm!(
                    "lhu {value}, 0({ptr})",
                    value = out(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u16, value: u16) {
            unsafe {
                asm!(
                    "sh {value}, 0({ptr})",
                    value = in(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
        }
    }

    impl MemAccess for u32 {
        unsafe fn load_relaxed(p: *const u32) -> u32 {
            let value: u32;
            unsafe {
                asm!(
                    "lwu {value}, 0({ptr})",
                    value = out(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u32, value: u32) {
            unsafe {
                asm!(
                    "sw {value}, 0({ptr})",
                    value = in(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
        }
    }

    impl MemAccess for u64 {
        unsafe fn load_relaxed(p: *const u64) -> u64 {
            let value: u64;
            unsafe {
                asm!(
                    "ld {value}, 0({ptr})",
                    value = out(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
            value
        }

        unsafe fn store_relaxed(p: *mut u64, value: u64) {
            unsafe {
                asm!(
                    "sd {value}, 0({ptr})",
                    value = in(reg) value,
                    ptr = in(reg) p,
                    options(nostack, preserves_flags),
                )
            };
        }
    }
}

/// A window of memory-mapped registers, mirroring libarch's `arch::base_mem_space`.
///
/// Accesses are ordered within the space's domain; `load_relaxed` and `store_relaxed` opt out of
/// that ordering. Since the effect of an access depends on the register that is addressed, all
/// accessors are unsafe; safe accessors for individual registers are built on top of them.
#[derive(Clone, Copy)]
pub struct BaseMemSpace<D: Domain> {
    base: *mut u8,
    len: usize,
    _domain: PhantomData<fn() -> D>,
}

// SAFETY: BaseMemSpace only stores a pointer to MMIO. Accesses are unsafe and it is up to the
// caller to synchronize them.
unsafe impl<D: Domain> Send for BaseMemSpace<D> {}
unsafe impl<D: Domain> Sync for BaseMemSpace<D> {}

impl<D: Domain> BaseMemSpace<D> {
    /// # Safety
    ///
    /// `base` has to point to `len` bytes of memory that outlive this space.
    pub unsafe fn new(base: *mut u8, len: usize) -> BaseMemSpace<D> {
        BaseMemSpace {
            base,
            len,
            _domain: PhantomData,
        }
    }

    /// Returns the window that starts at `offset` bytes into this window.
    pub fn subspace(&self, offset: usize) -> BaseMemSpace<D> {
        assert!(offset <= self.len);

        BaseMemSpace {
            // SAFETY: The offset stays within the window that we were constructed from.
            base: unsafe { self.base.add(offset) },
            len: self.len - offset,
            _domain: PhantomData,
        }
    }

    /// # Safety
    ///
    /// See [`BaseMemSpace`].
    pub unsafe fn load<R>(&self, reg: R) -> R::Repr
    where
        R: Register,
        R::Bits: MemAccess,
    {
        R::Repr::from_bits(unsafe { self.scalar_load::<R::Bits>(reg.offset()) })
    }

    /// # Safety
    ///
    /// See [`BaseMemSpace`].
    pub unsafe fn store<R>(&self, reg: R, value: R::Repr)
    where
        R: Register,
        R::Bits: MemAccess,
    {
        unsafe { self.scalar_store::<R::Bits>(reg.offset(), value.to_bits()) };
    }

    /// # Safety
    ///
    /// See [`BaseMemSpace`].
    pub unsafe fn load_relaxed<R>(&self, reg: R) -> R::Repr
    where
        R: Register,
        R::Bits: MemAccess,
    {
        R::Repr::from_bits(unsafe { self.scalar_load_relaxed::<R::Bits>(reg.offset()) })
    }

    /// # Safety
    ///
    /// See [`BaseMemSpace`].
    pub unsafe fn store_relaxed<R>(&self, reg: R, value: R::Repr)
    where
        R: Register,
        R::Bits: MemAccess,
    {
        unsafe { self.scalar_store_relaxed::<R::Bits>(reg.offset(), value.to_bits()) };
    }

    /// # Safety
    ///
    /// See [`BaseMemSpace`].
    pub unsafe fn scalar_load<T: MemAccess>(&self, offset: usize) -> T {
        D::before_load();
        let value = unsafe { self.scalar_load_relaxed::<T>(offset) };
        D::after_load();
        value
    }

    /// # Safety
    ///
    /// See [`BaseMemSpace`].
    pub unsafe fn scalar_store<T: MemAccess>(&self, offset: usize, value: T) {
        D::before_store();
        unsafe { self.scalar_store_relaxed::<T>(offset, value) };
        D::after_store();
    }

    /// # Safety
    ///
    /// See [`BaseMemSpace`].
    pub unsafe fn scalar_load_relaxed<T: MemAccess>(&self, offset: usize) -> T {
        let p = self.pointer_to::<T>(offset);

        unsafe { T::load_relaxed(p) }
    }

    /// # Safety
    ///
    /// See [`BaseMemSpace`].
    pub unsafe fn scalar_store_relaxed<T: MemAccess>(&self, offset: usize, value: T) {
        let p = self.pointer_to::<T>(offset);

        unsafe { T::store_relaxed(p, value) };
    }

    /// Whether an access of `T` at `offset` is naturally aligned and stays within the window.
    ///
    /// The accessors panic on an access that is not okay; callers that address the space from
    /// untrusted input reject the access instead.
    pub fn access_ok<T: MemAccess>(&self, offset: usize) -> bool {
        let Some(end) = offset.checked_add(size_of::<T>()) else {
            return false;
        };

        // Registers are addressed by naturally aligned accesses; a load or store of T would be
        // undefined behavior otherwise.
        end <= self.len
            && self
                .base
                .addr()
                .wrapping_add(offset)
                .is_multiple_of(align_of::<T>())
    }

    fn pointer_to<T: MemAccess>(&self, offset: usize) -> *mut T {
        assert!(
            self.access_ok::<T>(offset),
            "arch: memory access outside of the window"
        );

        // SAFETY: The access stays within the window that we were constructed from.
        unsafe { self.base.add(offset).cast::<T>() }
    }
}

/// A window of memory-mapped registers of a device.
pub type IoMemSpace = BaseMemSpace<IoMemDomain>;

/// A window of main memory that is shared with a device.
pub type MainMemSpace = BaseMemSpace<MainMemDomain>;
