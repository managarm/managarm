#ifndef LIBARCH_VARIABLE_HPP
#define LIBARCH_VARIABLE_HPP

#include <arch/bits.hpp>
#include <arch/mem_space.hpp>

namespace arch {

template<typename R, typename B>
struct basic_variable {
	using rep_type = R;
	using bits_type = B;

	basic_variable() = default;

	explicit constexpr basic_variable(R r)
	: _embedded{static_cast<B>(r)} { }

	R load() {
		return static_cast<R>(_detail::mem_ops<B>::load(&_embedded));
	}

	void store(R r) {
		_detail::mem_ops<B>::store(&_embedded, static_cast<B>(r));
	}

	R atomic_exchange(R r) {
		return static_cast<R>(_detail::mem_ops<B>::atomic_exchange(&_embedded, static_cast<B>(r)));
	}

private:
	B _embedded;
};

template<typename T>
using scalar_variable = basic_variable<T, T>;

template<typename B>
using bit_variable = basic_variable<bit_value<B>, B>;

} // namespace arch

#endif // LIBARCH_VARIABLE_HPP
