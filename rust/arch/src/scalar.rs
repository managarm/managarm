use core::fmt::Debug;
use core::ops::{BitAnd, BitOr, Not, Shl, Shr};

use crate::bits::BitConvertible;
use crate::sealed;

/// A type that a single memory or I/O transaction can carry.
pub trait Scalar:
    Copy
    + Eq
    + Debug
    + Not<Output = Self>
    + BitAnd<Output = Self>
    + BitOr<Output = Self>
    + Shl<u32, Output = Self>
    + Shr<u32, Output = Self>
    + BitConvertible<Self>
    + sealed::Sealed
{
    const ZERO: Self;
    const BITS: u32;
}

macro_rules! impl_bit_convertible {
    ($b:ty, $t:ty) => {
        impl BitConvertible<$b> for $t {
            fn from_bits(bits: $b) -> Self {
                bits as $t
            }

            fn to_bits(self) -> $b {
                self as $b
            }
        }
    };
}

macro_rules! impl_scalar {
    ($b:ty) => {
        impl sealed::Sealed for $b {}

        impl Scalar for $b {
            const ZERO: Self = 0;
            const BITS: u32 = <$b>::BITS;
        }

        impl BitConvertible<$b> for bool {
            fn from_bits(bits: $b) -> Self {
                bits != 0
            }

            fn to_bits(self) -> $b {
                self as $b
            }
        }

        impl_bit_convertible!($b, u8);
        impl_bit_convertible!($b, u16);
        impl_bit_convertible!($b, u32);
        impl_bit_convertible!($b, u64);
    };
}

impl_scalar!(u8);
impl_scalar!(u16);
impl_scalar!(u32);
impl_scalar!(u64);
