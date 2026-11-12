use core::marker::PhantomData;

use crate::bits::BitConvertible;
use crate::domain::{Domain, IoMemDomain, MainMemDomain};
use crate::register::Register;
use crate::{Scalar, sealed};

/// A type that a single memory transaction can carry.
///
/// The accesses are relaxed, i.e. they carry no ordering guarantees; [`BaseMemSpace`] combines
/// them with the barriers of its domain, mirroring how libarch builds its `*_mem_ops` on top of
/// `relaxed_mem_ops`. They are inline assembly and not volatile pointer accesses to guarantee
/// that the compiler emits neither an unexpected instruction nor a torn access.
pub trait MemAccess: Scalar {
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

/// Byte order of a device register, independent of the CPU's byte order.
pub trait ByteOrder: sealed::Sealed + Copy {
    fn convert<T: Scalar>(value: T) -> T;
}

#[derive(Clone, Copy)]
pub enum NativeEndian {}
#[derive(Clone, Copy)]
pub enum LittleEndian {}
#[derive(Clone, Copy)]
pub enum BigEndian {}
impl sealed::Sealed for NativeEndian {}
impl sealed::Sealed for LittleEndian {}
impl sealed::Sealed for BigEndian {}
impl ByteOrder for NativeEndian {
    fn convert<T: Scalar>(value: T) -> T {
        value
    }
}
impl ByteOrder for LittleEndian {
    fn convert<T: Scalar>(value: T) -> T {
        if cfg!(target_endian = "big") {
            value.swap_bytes()
        } else {
            value
        }
    }
}
impl ByteOrder for BigEndian {
    fn convert<T: Scalar>(value: T) -> T {
        if cfg!(target_endian = "little") {
            value.swap_bytes()
        } else {
            value
        }
    }
}

/// A window of memory-mapped registers, mirroring libarch's `arch::base_mem_space`.
///
/// Accesses are ordered within the space's domain; `load_relaxed` and `store_relaxed` opt out of
/// that ordering. Since the effect of an access depends on the register that is addressed, all
/// accessors are unsafe; safe accessors for individual registers are built on top of them.
#[derive(Clone, Copy)]
pub struct BaseMemSpace<D: Domain, O: ByteOrder = NativeEndian> {
    base: *mut u8,
    len: usize,
    _domain: PhantomData<fn() -> (D, O)>,
}

// SAFETY: BaseMemSpace only stores a pointer to MMIO. Accesses are unsafe and it is up to the
// caller to synchronize them.
unsafe impl<D: Domain, O: ByteOrder> Send for BaseMemSpace<D, O> {}
unsafe impl<D: Domain, O: ByteOrder> Sync for BaseMemSpace<D, O> {}

impl<D: Domain, O: ByteOrder> BaseMemSpace<D, O> {
    /// # Safety
    ///
    /// `base` has to point to `len` bytes of memory that outlive this space.
    pub unsafe fn new(base: *mut u8, len: usize) -> BaseMemSpace<D, O> {
        BaseMemSpace {
            base,
            len,
            _domain: PhantomData,
        }
    }

    /// Returns the window that starts at `offset` bytes into this window.
    pub fn subspace(&self, offset: usize) -> BaseMemSpace<D, O> {
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

        O::convert(unsafe { T::load_relaxed(p) })
    }

    /// # Safety
    ///
    /// See [`BaseMemSpace`].
    pub unsafe fn scalar_store_relaxed<T: MemAccess>(&self, offset: usize, value: T) {
        let p = self.pointer_to::<T>(offset);

        unsafe { T::store_relaxed(p, O::convert(value)) };
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

/// A window of little-endian device registers, such as PCI configuration space.
pub type LittleEndianIoMemSpace = BaseMemSpace<IoMemDomain, LittleEndian>;

/// A window of big-endian device registers, such as sun4u host bridge registers.
pub type BigEndianIoMemSpace = BaseMemSpace<IoMemDomain, BigEndian>;

/// A window of main memory that is shared with a device.
pub type MainMemSpace = BaseMemSpace<MainMemDomain>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn device_byte_orders() {
        let mut backing = [0u64; 2];
        let base = backing.as_mut_ptr().cast::<u8>();
        let native = unsafe { IoMemSpace::new(base, 16) };
        let little = unsafe { LittleEndianIoMemSpace::new(base, 16) };
        let big = unsafe { BigEndianIoMemSpace::new(base, 16) };
        unsafe {
            little.scalar_store::<u32>(0, 0x12345678);
            assert_eq!(
                core::slice::from_raw_parts(base, 4),
                &[0x78, 0x56, 0x34, 0x12]
            );
            assert_eq!(little.scalar_load::<u32>(0), 0x12345678);
            assert_eq!(big.scalar_load::<u32>(0), 0x78563412);
            big.scalar_store::<u64>(8, 0x123456789abcdef0);
            assert_eq!(
                core::slice::from_raw_parts(base.add(8), 8),
                &[0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0]
            );
            assert_eq!(big.subspace(8).scalar_load::<u64>(0), 0x123456789abcdef0);
            native.scalar_store::<u16>(0, 0x1234);
            assert_eq!(
                core::slice::from_raw_parts(base, 2),
                &0x1234u16.to_ne_bytes()
            );
            assert_eq!(little.access_ok::<u32>(1), false);
        }
    }

    crate::scalar_register!(Data @ 0x00: u32);
    crate::bit_register! {
        Control @ 0x08: u32 {
            ENABLE @ 0, 1: bool;
            MODE @ 16, 8: u8;
        }
    }

    #[test]
    fn relaxed_access_widths() {
        let mut backing = [0u64; 1];
        let base = backing.as_mut_ptr().cast::<u8>();
        let little = unsafe { LittleEndianIoMemSpace::new(base, 8) };
        let big = unsafe { BigEndianIoMemSpace::new(base, 8) };
        macro_rules! check {
            ($ty:ty, $value:expr) => {
                let value: $ty = $value;
                unsafe {
                    little.scalar_store_relaxed::<$ty>(0, value);
                    assert_eq!(
                        core::slice::from_raw_parts(base, size_of::<$ty>()),
                        &value.to_le_bytes()
                    );
                    assert_eq!(little.scalar_load_relaxed::<$ty>(0), value);
                    big.scalar_store_relaxed::<$ty>(0, value);
                    assert_eq!(
                        core::slice::from_raw_parts(base, size_of::<$ty>()),
                        &value.to_be_bytes()
                    );
                    assert_eq!(big.scalar_load_relaxed::<$ty>(0), value);
                }
            };
        }
        check!(u8, 0x12);
        check!(u16, 0x1234);
        check!(u32, 0x12345678);
        check!(u64, 0x123456789abcdef0);
    }

    #[test]
    fn typed_registers() {
        let mut backing = [0u64; 2];
        let base = backing.as_mut_ptr().cast::<u8>();
        let little = unsafe { LittleEndianIoMemSpace::new(base, 16) };
        let big = unsafe { BigEndianIoMemSpace::new(base, 16) };
        let control = crate::BitValue::<Control>::zero()
            .with(Control::ENABLE, true)
            .with(Control::MODE, 0x42);
        unsafe {
            little.store(Data, 0x12345678);
            assert_eq!(little.load(Data), 0x12345678);
            assert_eq!(big.load(Data), 0x78563412);
            little.store(Control, control);
            assert_eq!(
                core::slice::from_raw_parts(base.add(8), 4),
                &[1, 0, 0x42, 0]
            );
            assert!(little.load(Control).get(Control::ENABLE));
            assert_eq!(little.load(Control).get(Control::MODE), 0x42);
            big.store_relaxed(Control, control);
            assert_eq!(
                core::slice::from_raw_parts(base.add(8), 4),
                &[0, 0x42, 0, 1]
            );
            assert!(big.load_relaxed(Control).get(Control::ENABLE));
            assert_eq!(big.load_relaxed(Control).get(Control::MODE), 0x42);
            assert_eq!(big.subspace(8).load(Data), control.raw());
        }
    }
}
