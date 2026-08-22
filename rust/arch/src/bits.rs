use core::fmt;
use core::marker::PhantomData;

use crate::register::Register;
use crate::scalar::Scalar;

/// A conversion between the value that a field or register holds and its bits.
///
/// This mirrors the `static_cast`s that libarch performs between a register's `rep_type` and its
/// `bits_type`. Drivers implement it to give a field a domain-specific value type.
pub trait BitConvertible<B: Scalar>: Copy {
    fn from_bits(bits: B) -> Self;
    fn to_bits(self) -> B;
}

/// A vector of bits that covers a whole register, mirroring libarch's `arch::bit_value`.
///
/// Unlike a scalar register's value, a bit value is opaque: it is addressed through the fields of
/// the register instead of through arithmetic.
#[derive(Clone, Copy)]
pub struct BitValue<R: Register> {
    bits: R::Bits,
}

impl<R: Register> BitValue<R> {
    pub fn new(bits: R::Bits) -> BitValue<R> {
        BitValue { bits }
    }

    /// The all-clear value, as the base for composing a whole register from its fields.
    pub fn zero() -> BitValue<R> {
        BitValue {
            bits: <R::Bits as Scalar>::ZERO,
        }
    }

    pub fn raw(self) -> R::Bits {
        self.bits
    }

    /// Extracts `field` from this value.
    pub fn get<T: BitConvertible<R::Bits>>(self, field: Field<R, T>) -> T {
        field.get(self.bits)
    }

    /// Replaces `field` by `value`, leaving every other bit alone. libarch spells this
    /// `operator/`.
    #[must_use = "with returns the updated value instead of modifying in place"]
    pub fn with<T: BitConvertible<R::Bits>>(self, field: Field<R, T>, value: T) -> BitValue<R> {
        let mask = field.mask();

        BitValue {
            bits: (self.bits & !mask) | ((value.to_bits() << field.shift) & mask),
        }
    }
}

impl<R: Register> BitConvertible<R::Bits> for BitValue<R> {
    fn from_bits(bits: R::Bits) -> Self {
        BitValue { bits }
    }

    fn to_bits(self) -> R::Bits {
        self.bits
    }
}

impl<R: Register> fmt::Debug for BitValue<R> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_tuple("BitValue").field(&self.bits).finish()
    }
}

impl<R: Register> PartialEq for BitValue<R> {
    fn eq(&self, other: &Self) -> bool {
        self.bits == other.bits
    }
}

impl<R: Register> Eq for BitValue<R> {}

/// A bit field of a register, mirroring libarch's `arch::field`.
///
/// `R` is the register that the field lives in, `T` is the type of the value that the field
/// holds.
#[derive(Clone, Copy)]
pub struct Field<R: Register, T = <R as Register>::Bits> {
    shift: u32,
    bits: u32,
    _value: PhantomData<fn() -> (R, T)>,
}

impl<R: Register, T: BitConvertible<R::Bits>> Field<R, T> {
    /// Declaring a field that overhangs its register fails `const` evaluation, i.e. at compile
    /// time for the usual `const` field declarations.
    pub const fn new(shift: u32, bits: u32) -> Field<R, T> {
        assert!(bits > 0 && shift + bits <= <R::Bits as Scalar>::BITS);

        Field {
            shift,
            bits,
            _value: PhantomData,
        }
    }

    fn mask(self) -> R::Bits {
        (!<R::Bits as Scalar>::ZERO >> (<R::Bits as Scalar>::BITS - self.bits)) << self.shift
    }

    /// Extracts this field from the bits of a register.
    pub fn get(self, value: R::Bits) -> T {
        T::from_bits((value & self.mask()) >> self.shift)
    }
}

impl<R: Register, T> fmt::Debug for Field<R, T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Field")
            .field("shift", &self.shift)
            .field("bits", &self.bits)
            .finish()
    }
}
