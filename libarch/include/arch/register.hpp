
#ifndef LIBARCH_REGISTER_HPP
#define LIBARCH_REGISTER_HPP

#include <stddef.h>

#include <arch/bits.hpp>

namespace arch {

template<typename R, typename B, typename P = ptrdiff_t>
struct basic_register {
	using rep_type = R;
	using bits_type = B;

	explicit constexpr basic_register(P offset)
	: _offset(offset) { }

	P offset() const {
		return _offset;
	}

private:
	P _offset;
};

template<typename T, typename P = ptrdiff_t>
using scalar_register = basic_register<T, T, P>;

template<typename B, typename P = ptrdiff_t>
using bit_register = basic_register<bit_value<B>, B, P>;

} // namespace arch

#endif // LIBARCH_REGISTER_HPP

