
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

// Space is second because otherwise you can't do scalar_load<uint32_t> etc
template<typename T, typename Space>
T scalar_load(Space &s, ptrdiff_t offset) {
	return s.load(scalar_register<T>(offset));
}

// Same as above
template<typename T, typename Space>
void scalar_store(Space &s, ptrdiff_t offset, T val) {
	return s.store(scalar_register<T>(offset), val);
}

} // namespace arch

#endif // LIBARCH_REGISTER_HPP

