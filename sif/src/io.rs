#[cfg(target_arch = "x86_64")]
mod internal {
    use core::arch::asm;

    pub unsafe fn outb(port: u16, value: u8) {
        unsafe {
            asm!("out dx, al", in("dx") port, in("al") value, options(nomem, nostack, preserves_flags))
        };
    }
    pub unsafe fn outw(port: u16, value: u16) {
        unsafe {
            asm!("out dx, ax", in("dx") port, in("ax") value, options(nomem, nostack, preserves_flags))
        };
    }
    pub unsafe fn outl(port: u16, value: u32) {
        unsafe {
            asm!("out dx, eax", in("dx") port, in("eax") value, options(nomem, nostack, preserves_flags))
        };
    }
    pub unsafe fn inb(port: u16) -> u8 {
        let value: u8;
        unsafe {
            asm!("in al, dx", out("al") value, in("dx") port, options(nomem, nostack, preserves_flags))
        };
        value
    }
    pub unsafe fn inw(port: u16) -> u16 {
        let value: u16;
        unsafe {
            asm!("in ax, dx", out("ax") value, in("dx") port, options(nomem, nostack, preserves_flags))
        };
        value
    }
    pub unsafe fn inl(port: u16) -> u32 {
        let value: u32;
        unsafe {
            asm!("in eax, dx", out("eax") value, in("dx") port, options(nomem, nostack, preserves_flags))
        };
        value
    }
}

#[cfg(not(target_arch = "x86_64"))]
mod internal {
    pub unsafe fn outb(_port: u16, _value: u8) {}
    pub unsafe fn outw(_port: u16, _value: u16) {}
    pub unsafe fn outl(_port: u16, _value: u32) {}
    pub unsafe fn inb(_port: u16) -> u8 {
        0xFF
    }
    pub unsafe fn inw(_port: u16) -> u16 {
        0xFFFF
    }
    pub unsafe fn inl(_port: u16) -> u32 {
        0xFFFFFFFF
    }
}

pub use internal::*;
