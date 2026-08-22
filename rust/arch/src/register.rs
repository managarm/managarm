use crate::bits::BitConvertible;
use crate::scalar::Scalar;

/// A register of a memory or I/O space.
///
/// Unlike libarch's `arch::basic_register`, a register is identified by its type: fields are tied
/// to the register that they belong to, so that reading a field out of an unrelated register's
/// value does not compile. The offset is a property of the register value rather than the type, so
/// that a register kind can be placed at a runtime-determined offset.
pub trait Register: Copy {
    /// The width of the access that reads or writes the register.
    type Bits: Scalar;

    /// The type that the register's value is represented by: `Self::Bits` for a register that
    /// holds a plain value, `BitValue<Self>` for a register that is addressed through its fields.
    type Repr: BitConvertible<Self::Bits>;

    fn offset(self) -> usize;
}

/// Declares a register that is addressed through its fields, placed at a fixed offset.
///
/// ```
/// arch::bit_register! {
///     BridgeCtl @ 0x04: u32 {
///         RESET @ 0, 1: bool;
///         MAX_BURST_SIZE @ 20, 2: u8;
///     }
/// }
/// ```
#[macro_export]
macro_rules! bit_register {
    ($(#[$meta:meta])* $vis:vis $name:ident @ $offset:literal: $bits:ty {
        $($(#[$fmeta:meta])* $field:ident @ $shift:literal, $width:literal: $t:ty;)*
    }) => {
        $(#[$meta])*
        #[derive(Clone, Copy)]
        $vis struct $name;

        impl $crate::Register for $name {
            type Bits = $bits;
            type Repr = $crate::BitValue<Self>;

            fn offset(self) -> usize {
                $offset
            }
        }

        // Registers transcribe the datasheet, so fields may be declared ahead of any use.
        #[allow(dead_code)]
        impl $name {
            $($(#[$fmeta])* $vis const $field: $crate::Field<Self, $t> =
                $crate::Field::new($shift, $width);)*
        }
    };
}

/// Declares a register that is addressed through its fields, placed at a runtime-determined
/// offset: each instance carries its own offset, while all instances share the fields.
///
/// ```
/// arch::runtime_bit_register! {
///     ChanCtl: u32 {
///         ENABLE @ 0, 1: bool;
///     }
/// }
/// let chan2 = ChanCtl::at(0x10 + 2 * 4);
/// ```
#[macro_export]
macro_rules! runtime_bit_register {
    ($(#[$meta:meta])* $vis:vis $name:ident: $bits:ty {
        $($(#[$fmeta:meta])* $field:ident @ $shift:literal, $width:literal: $t:ty;)*
    }) => {
        $(#[$meta])*
        #[derive(Clone, Copy)]
        $vis struct $name {
            offset: usize,
        }

        // Registers transcribe the datasheet, so fields may be declared ahead of any use.
        #[allow(dead_code)]
        impl $name {
            /// Places the register at `offset`.
            $vis const fn at(offset: usize) -> Self {
                Self { offset }
            }

            $($(#[$fmeta])* $vis const $field: $crate::Field<Self, $t> =
                $crate::Field::new($shift, $width);)*
        }

        impl $crate::Register for $name {
            type Bits = $bits;
            type Repr = $crate::BitValue<Self>;

            fn offset(self) -> usize {
                self.offset
            }
        }
    };
}

/// Declares a register that holds a plain value, placed at a fixed offset.
///
/// ```
/// arch::scalar_register!(HwRev @ 0x00: u32);
/// ```
#[macro_export]
macro_rules! scalar_register {
    ($(#[$meta:meta])* $vis:vis $name:ident @ $offset:literal: $bits:ty) => {
        $(#[$meta])*
        #[derive(Clone, Copy)]
        $vis struct $name;

        impl $crate::Register for $name {
            type Bits = $bits;
            type Repr = $bits;

            fn offset(self) -> usize {
                $offset
            }
        }
    };
}

/// Declares a register that holds a plain value, placed at a runtime-determined offset: each
/// instance carries its own offset.
///
/// ```
/// arch::runtime_scalar_register!(ChanData: u32);
/// let chan2 = ChanData::at(0x10 + 2 * 4);
/// ```
#[macro_export]
macro_rules! runtime_scalar_register {
    ($(#[$meta:meta])* $vis:vis $name:ident: $bits:ty) => {
        $(#[$meta])*
        #[derive(Clone, Copy)]
        $vis struct $name {
            offset: usize,
        }

        impl $name {
            /// Places the register at `offset`.
            $vis const fn at(offset: usize) -> Self {
                Self { offset }
            }
        }

        impl $crate::Register for $name {
            type Bits = $bits;
            type Repr = $bits;

            fn offset(self) -> usize {
                self.offset
            }
        }
    };
}
