use crate::bits::BitConvertible;
use crate::register::Register;
use crate::sealed;

/// A type that a single port I/O transaction can carry.
pub trait PioAccess: sealed::Sealed {
    /// # Safety
    ///
    /// See [`PioSpace`].
    unsafe fn load(addr: u16) -> Self;

    /// # Safety
    ///
    /// See [`PioSpace`].
    unsafe fn store(addr: u16, value: Self);
}

#[cfg(target_arch = "x86_64")]
mod internal {
    use core::arch::asm;

    use super::PioAccess;

    impl PioAccess for u8 {
        unsafe fn load(addr: u16) -> u8 {
            let value: u8;
            unsafe {
                asm!("in al, dx", out("al") value, in("dx") addr, options(nostack, preserves_flags))
            };
            value
        }

        unsafe fn store(addr: u16, value: u8) {
            unsafe {
                asm!("out dx, al", in("dx") addr, in("al") value, options(nostack, preserves_flags))
            };
        }
    }

    impl PioAccess for u16 {
        unsafe fn load(addr: u16) -> u16 {
            let value: u16;
            unsafe {
                asm!("in ax, dx", out("ax") value, in("dx") addr, options(nostack, preserves_flags))
            };
            value
        }

        unsafe fn store(addr: u16, value: u16) {
            unsafe {
                asm!("out dx, ax", in("dx") addr, in("ax") value, options(nostack, preserves_flags))
            };
        }
    }

    impl PioAccess for u32 {
        unsafe fn load(addr: u16) -> u32 {
            let value: u32;
            unsafe {
                asm!("in eax, dx", out("eax") value, in("dx") addr, options(nostack, preserves_flags))
            };
            value
        }

        unsafe fn store(addr: u16, value: u32) {
            unsafe {
                asm!("out dx, eax", in("dx") addr, in("eax") value, options(nostack, preserves_flags))
            };
        }
    }
}

// Unlike libarch, which traps here, managarm degrades port I/O on architectures that do not have
// it: reads return all-ones and writes are dropped, so that an ACPI SystemIO operation region
// reads as an absent device instead of taking the server down.
#[cfg(not(target_arch = "x86_64"))]
mod internal {
    use super::PioAccess;

    impl PioAccess for u8 {
        unsafe fn load(_addr: u16) -> u8 {
            0xFF
        }

        unsafe fn store(_addr: u16, _value: u8) {}
    }

    impl PioAccess for u16 {
        unsafe fn load(_addr: u16) -> u16 {
            0xFFFF
        }

        unsafe fn store(_addr: u16, _value: u16) {}
    }

    impl PioAccess for u32 {
        unsafe fn load(_addr: u16) -> u32 {
            0xFFFFFFFF
        }

        unsafe fn store(_addr: u16, _value: u32) {}
    }
}

/// A window of I/O ports, mirroring libarch's `arch::io_space`.
///
/// Since the effect of an access depends on the register that is addressed, all accessors are
/// unsafe; safe accessors for individual registers are built on top of them.
#[derive(Clone, Copy)]
pub struct PioSpace {
    base: u16,
}

impl PioSpace {
    /// # Safety
    ///
    /// The caller has to have been granted access to the ports of this window.
    pub const unsafe fn new(base: u16) -> PioSpace {
        PioSpace { base }
    }

    /// Returns the window that starts at `offset` ports into this window.
    pub fn subspace(&self, offset: u16) -> PioSpace {
        PioSpace {
            base: self
                .base
                .checked_add(offset)
                .expect("arch: I/O subspace overflows the 16-bit port space"),
        }
    }

    /// # Safety
    ///
    /// See [`PioSpace`].
    pub unsafe fn load<R>(&self, reg: R) -> R::Repr
    where
        R: Register,
        R::Bits: PioAccess,
    {
        R::Repr::from_bits(unsafe { self.scalar_load::<R::Bits>(reg.offset()) })
    }

    /// # Safety
    ///
    /// See [`PioSpace`].
    pub unsafe fn store<R>(&self, reg: R, value: R::Repr)
    where
        R: Register,
        R::Bits: PioAccess,
    {
        unsafe { self.scalar_store::<R::Bits>(reg.offset(), value.to_bits()) };
    }

    /// # Safety
    ///
    /// See [`PioSpace`].
    pub unsafe fn scalar_load<T: PioAccess>(&self, offset: usize) -> T {
        unsafe { <T as PioAccess>::load(self.port::<T>(offset)) }
    }

    /// # Safety
    ///
    /// See [`PioSpace`].
    pub unsafe fn scalar_store<T: PioAccess>(&self, offset: usize, value: T) {
        unsafe { <T as PioAccess>::store(self.port::<T>(offset), value) };
    }

    /// Whether an access of `T` at `offset` stays within the 16-bit port space.
    ///
    /// The accessors panic on an access that is not okay; callers that address the space from
    /// untrusted input reject the access instead.
    pub fn access_ok<T: PioAccess>(&self, offset: usize) -> bool {
        self.checked_port::<T>(offset).is_some()
    }

    fn checked_port<T: PioAccess>(&self, offset: usize) -> Option<u16> {
        let port = (self.base as usize).checked_add(offset)?;

        // The whole access has to fit, not just the port that it starts at.
        (port.checked_add(size_of::<T>())? <= 1 << 16).then_some(port as u16)
    }

    fn port<T: PioAccess>(&self, offset: usize) -> u16 {
        self.checked_port::<T>(offset)
            .expect("arch: port I/O access outside the 16-bit port space")
    }
}
