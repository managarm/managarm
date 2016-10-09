
#ifndef LIBARCH_BITS_HPP
#define LIBARCH_BITS_HPP

namespace arch {

template<typename B>
struct bit_mask {
	explicit bit_mask(B bits)
	: _bits(bits) { }

	explicit operator B () {
		return _bits;
	}

private:
	B _bits;
};

// represents a fixed size vector of bits.
template<typename B>
struct bit_value {
	explicit bit_value(B bits)
	: _bits(bits) { }

	explicit operator B () {
		return _bits;
	}

	// allow building a value from multiple bit vectors.
	bit_value operator| (bit_value other) const {
		return bit_value(_bits | other._bits);
	}

	// allow masking out individual bits.
	bit_value operator& (bit_mask<B> other) const {
		return bit_value(_bits & static_cast<B>(other));
	}

private:
	B _bits;
};

template<typename B, typename T>
struct field {
	// allow extraction of individual fields from bit vectors.
	friend T operator& (bit_value<B> bv, field f) {
		return static_cast<T>((static_cast<B>(bv) >> f._shift) & f._mask);
	}

	explicit constexpr field(int shift, B mask)
	: _shift(shift), _mask(mask) { }

	// allow construction of bit vectors from fields.
	bit_value<B> operator() (T value) const {
		return bit_value<B>((static_cast<B>(value) & _mask) << _shift);
	}

	// allow inversion of this field to a bit mask.
	bit_mask<B> operator~ () const {
		return bit_mask<B>(~(_mask << _shift));
	}

private:
	int _shift;
	B _mask;
};

} // namespace arch

#endif // LIBARCH_BITS_HPP
