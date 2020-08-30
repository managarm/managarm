
#ifndef FRIGG_VARIANT_HPP
#define FRIGG_VARIANT_HPP

#include <type_traits>
#include <frigg/macros.hpp>
#include <frigg/c-support.h>

namespace frigg FRIGG_VISIBILITY {

namespace _variant {
	// check if S is one of the types T
	template<typename S, typename... T>
	struct Exists
	: public std::false_type { };

	template<typename S, typename... T>
	struct Exists<S, S, T...>
	: public std::true_type { };

	template<typename S, typename H, typename... T>
	struct Exists<S, H, T...>
	: public Exists<S, T...> { };

	// get the index of S in the argument pack T
	template<typename, typename S, typename... T>
	struct IndexOfHelper { };
	
	template<typename S, typename... T>
	struct IndexOfHelper<std::enable_if_t<Exists<S, S, T...>::value>, S, S, T...>
	: public std::integral_constant<size_t, 0> { };

	template<typename S, typename H, typename... T>
	struct IndexOfHelper<std::enable_if_t<Exists<S, H, T...>::value>, S, H, T...>
	: public std::integral_constant<size_t, IndexOfHelper<void, S, T...>::value + 1> { };

	template<typename S, typename... T>
	using IndexOf = IndexOfHelper<void, S, T...>;

	// get a type with a certain index from the argument pack T
	template<size_t Index, typename... T>
	struct GetHelper { };

	template<typename H, typename... T>
	struct GetHelper<0, H, T...> {
		using Type = H;
	};

	template<size_t Index, typename H, typename... T>
	struct GetHelper<Index, H, T...>
	: public GetHelper<Index - 1, T...> { };

	template<size_t Index, typename... T>
	using Get = typename GetHelper<Index, T...>::Type;
};

template<typename... T>
struct Variant {
	template<typename X, size_t Index = _variant::IndexOf<X, T...>::value>
	static constexpr size_t tagOf() {
		return Index;
	}

	Variant()
	: _tag(-1) { }

	template<typename X, size_t Index = _variant::IndexOf<X, T...>::value>
	Variant(X object)
	: Variant() {
		_construct<Index>(std::move(object));
	};

	Variant(const Variant &other)
	: Variant() {
		if(other)
			_copyConstruct<0>(other);
	}
	Variant(Variant &&other)
	: Variant() {
		if(other)
			_moveConstruct<0>(std::move(other));
	}

	~Variant() {
		if(*this)
			_destruct<0>();
	}

	explicit operator bool() const {
		return _tag != -1;
	}

	Variant &operator= (Variant other) {
		// Because swap is quite hard to implement for this type we don't use copy-and-swap.
		// Instead we perform a destruct-then-move-construct operation on the internal object.
		// Note that we take the argument by value so there are no self-assignment problems.
		if(_tag == other._tag) {
			_assign<0>(std::move(other));
		}else{
			if(*this)
				_destruct<0>();
			if(other)
				_moveConstruct<0>(std::move(other));
		}
		return *this;
	}

	size_t tag() {
		return _tag;
	}

	template<typename X, size_t Index = _variant::IndexOf<X, T...>::value>
	bool is() const {
		return _tag == Index;
	}

	template<typename X, size_t Index = _variant::IndexOf<X, T...>::value>
	X &get() {
		assert(_tag == Index);
		return *reinterpret_cast<X *>(_access());
	}
	template<typename X, size_t Index = _variant::IndexOf<X, T...>::value>
	const X &get() const {
		assert(_tag == Index);
		return *reinterpret_cast<const X *>(_access());
	}

	template<typename X, size_t Index = _variant::IndexOf<X, T...>::value,
			typename... Args>
	void emplace(Args &&... args) {
		if(_tag != -1)
			_destruct<0>();
		new (_access()) X(std::forward<Args>(args)...);
		_tag = Index;
	}

	template<typename F>
	std::common_type_t<std::invoke_result_t<F, T&>...> apply(F functor) {
		return _apply<F, 0>(std::move(functor));
	}

	template<typename F>
	std::common_type_t<std::invoke_result_t<F, const T&>...> const_apply(F functor) const {
		return _apply<F, 0>(std::move(functor));
	}

private:
	void *_access() {
		return _storage.buffer;
	}
	const void *_access() const {
		return _storage.buffer;
	}

	// construct the internal object from one of the summed types
	template<size_t Index, typename X = _variant::Get<Index, T...>>
	void _construct(X object) {
		assert(!*this);
		new (_access()) X(std::move(object));
		_tag = Index;
	}

	// construct the internal object by copying from another variant
	template<size_t Index> requires (Index < sizeof...(T))
	void _copyConstruct(const Variant &other) {
		using value_type = _variant::Get<Index, T...>;
		if(other._tag == Index) {
			assert(!*this);
			new (_access()) value_type(other.get<value_type>());
			_tag = Index;
		} else {
			_copyConstruct<Index + 1>(other);
		}
	}

	template<size_t Index> requires (Index == sizeof...(T))
	void _copyConstruct(const Variant &) {
		assert(!"Copy-construction from variant with illegal tag");
	}

	// construct the internal object by moving from another variant
	template<size_t Index> requires (Index < sizeof...(T))
	void _moveConstruct(Variant &&other) {
		using value_type = _variant::Get<Index, T...>;
		if(other._tag == Index) {
			assert(!*this);
			new (_access()) value_type(std::move(other.get<value_type>()));
			_tag = Index;
		}else{
			_moveConstruct<Index + 1>(std::move(other));
		}
	}

	template<size_t Index> requires (Index == sizeof...(T))
	void _moveConstruct(Variant &&) {
		assert(!"Move-construction from variant with illegal tag");
	}

	// destruct the internal object
	template<size_t Index> requires (Index < sizeof...(T))
	void _destruct() {
		using value_type = _variant::Get<Index, T...>;
		if(_tag == Index) {
			get<value_type>().~value_type();
			_tag = -1;
		}else{
			_destruct<Index + 1>();
		}
	}

	template<size_t Index> requires (Index == sizeof...(T))
	void _destruct() {
		assert(!"Destruction of variant with illegal tag");
	}

	// assign the internal object
	template<size_t Index> requires (Index < sizeof...(T))
	void _assign(Variant other) {
		using value_type = _variant::Get<Index, T...>;
		if(_tag == Index) {
			get<value_type>() = std::move(other.get<value_type>());
		}else{
			_assign<Index + 1>(std::move(other));
		}
	}

	template<size_t Index> requires (Index == sizeof...(T))
	void _assign(Variant) {
		assert(!"Assignment from variant with illegal tag");
	}

	// apply a functor to the internal object
	template<typename F, size_t Index> requires (Index < sizeof...(T))
	std::common_type_t<std::invoke_result_t<F, T&>...>
	_apply(F functor) {
		using value_type = _variant::Get<Index, T...>;
		if(_tag == Index) {
			return functor(get<value_type>());
		}else{
			return _apply<F, Index + 1>(std::move(functor));
		}
	}

	template<typename F, size_t Index> requires (Index == sizeof...(T))
	std::common_type_t<std::invoke_result_t<F, T&>...>
	_apply(F) {
		assert(!"_apply() on variant with illegal tag");
		__builtin_unreachable();
	}

	template<typename F, size_t Index> requires (Index < sizeof...(T))
	std::common_type_t<std::invoke_result_t<F, const T&>...>
	const_apply(F functor) const {
		using value_type = _variant::Get<Index, T...>;
		if(_tag == Index) {
			return functor(get<value_type>());
		}else{
			return _apply<Index + 1>(std::move(functor));
		}
	}

	template<typename F, size_t Index> requires (Index == sizeof...(T))
	std::common_type_t<std::invoke_result_t<F, const T&>...>
	const_apply(F) const {
		assert(!"_apply() on variant with illegal tag");
		__builtin_unreachable();
	}

	size_t _tag;
	frg::aligned_union<T...> _storage;
};

} // namespace frigg

#endif // FRIGG_VARIANT_HPP

