
#ifndef FRIGG_TRAITS_HPP
#define FRIGG_TRAITS_HPP

#include <frigg/macros.hpp>
#include <frigg/cxx-support.hpp>

namespace frigg FRIGG_VISIBILITY {

template<typename... Types>
struct TypePack;

template<typename T, T Value>
struct IntegralConstant {
	static constexpr T value = Value;
};

struct TrueType : public IntegralConstant<bool, true> { };
struct FalseType : public IntegralConstant<bool ,false> { };

template<typename T>
T declval();

namespace _detail {
	template<typename... T>
	struct ToVoidHelper {
		using Type = void;
	};
};

template<typename... T>
using ToVoid = typename _detail::ToVoidHelper<T...>::Type;

namespace _detail {
	template<typename S, typename = void>
	struct ResultOfHelper { };

	template<typename F, typename... Args>
	struct ResultOfHelper<F(Args...), ToVoid<decltype(declval<F>() (declval<Args>()...))>> {
		using Type = decltype(declval<F>() (declval<Args>()...));
	};
};

template<typename T>
using ResultOf = typename _detail::ResultOfHelper<T>::Type;

template<bool condition, typename T = void>
struct EnableIf;

template<typename T>
struct EnableIf<true, T> {
	typedef T type;
};

template<bool condition, typename T = void>
using EnableIfT = typename EnableIf<condition, T>::type;

template<typename T>
struct IsIntegral {
	static constexpr bool value = false;
};

template<> struct IsIntegral<char> { static constexpr bool value = true; };
template<> struct IsIntegral<int> { static constexpr bool value = true; };
template<> struct IsIntegral<short> { static constexpr bool value = true; };
template<> struct IsIntegral<long> { static constexpr bool value = true; };
template<> struct IsIntegral<long long> { static constexpr bool value = true; };
template<> struct IsIntegral<unsigned char> { static constexpr bool value = true; };
template<> struct IsIntegral<unsigned int> { static constexpr bool value = true; };
template<> struct IsIntegral<unsigned short> { static constexpr bool value = true; };
template<> struct IsIntegral<unsigned long> { static constexpr bool value = true; };
template<> struct IsIntegral<unsigned long long> { static constexpr bool value = true; };

template<typename T>
struct IsSigned {
	static constexpr bool value = false;
};

template<> struct IsSigned<char> { static constexpr bool value = true; };
template<> struct IsSigned<int> { static constexpr bool value = true; };
template<> struct IsSigned<short> { static constexpr bool value = true; };
template<> struct IsSigned<long> { static constexpr bool value = true; };
template<> struct IsSigned<long long> { static constexpr bool value = true; };

template<typename T>
struct IsUnsigned {
	static constexpr bool value = false;
};

template<> struct IsUnsigned<unsigned char> { static constexpr bool value = true; };
template<> struct IsUnsigned<unsigned int> { static constexpr bool value = true; };
template<> struct IsUnsigned<unsigned short> { static constexpr bool value = true; };
template<> struct IsUnsigned<unsigned long> { static constexpr bool value = true; };
template<> struct IsUnsigned<unsigned long long> { static constexpr bool value = true; };

template<typename T>
struct RemoveRef {
	typedef T type;
};
template<typename T>
struct RemoveRef<T &> {
	typedef T type;
};
template<typename T>
struct RemoveRef<T &&> {
	typedef T type;
};

template<typename T> 
typename RemoveRef<T>::type &&move(T &&arg) noexcept {
	return static_cast<typename RemoveRef<T>::type &&>(arg);
} 

template<typename T>
T &&forward(typename RemoveRef<T>::type &arg) noexcept {
	return static_cast<T &&>(arg);
}

template<size_t... I>
struct IndexSequence { };

namespace _index_sequence {
	template<size_t N, size_t... Suffix>
	struct MakeHelper {
		using Type = typename MakeHelper<N - 1, N - 1, Suffix...>::Type;
	};

	template<size_t... Suffix>
	struct MakeHelper<0, Suffix...> {
		using Type = IndexSequence<Suffix...>;
	};
};

template<size_t N>
using MakeIndexSequence = typename _index_sequence::MakeHelper<N>::Type;

template<typename... T>
using IndexSequenceFor = MakeIndexSequence<sizeof...(T)>;

// This is a quick std::is_convertible implementation that works
// at least for pointers. I did not validate its correctness for all
// cases or compare it to other implementations; it is likely that this
// code is just plain wrong for many cases.
namespace _convertible {
	template<typename T>
	void test(T);

	template<typename From, typename To>
	using Helper = decltype(test<To>(declval<From>()));
};

template<typename From, typename To, typename = void>
struct IsConvertible
: public FalseType { };

template<typename From, typename To>
struct IsConvertible<From, To, _convertible::Helper<From, To>>
: public TrueType { };

namespace _common_type {
	template<typename... T>
	struct Helper;

	template<typename H>
	struct Helper<H> {
		using Type = H;
	};

	template<typename H, typename... T>
	struct Helper<H, T...> {
		using Type = decltype(declval<bool>() ? declval<H>()
				: declval<typename Helper<T...>::Type>());
	};
};

template<typename... T>
using CommonType = typename _common_type::Helper<T...>::Type;

template<size_t Size, size_t Align>
struct alignas(Align) AlignedStorage {
	char buffer[Size];
};

namespace _aligned_union {
	template<size_t... S>
	struct Max;

	template<size_t H>
	struct Max<H> : public IntegralConstant<size_t, H> { };
	
	template<size_t H, size_t... T>
	struct Max<H, T...> : public IntegralConstant<size_t,
			H >= Max<T...>::value ? H : Max<T...>::value> { };
};

template<typename... T>
using AlignedUnion = AlignedStorage<
	_aligned_union::Max<sizeof(T)...>::value,
	_aligned_union::Max<alignof(T)...>::value
>;

} // namespace frigg

#endif // FRIGG_TRAITS_HPP

