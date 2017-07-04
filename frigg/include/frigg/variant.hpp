
#ifndef FRIGG_VARIANT_HPP
#define FRIGG_VARIANT_HPP

#include <frigg/macros.hpp>
#include <frigg/traits.hpp>

namespace frigg FRIGG_VISIBILITY {

namespace _variant {
	// check if S is one of the types T
	template<typename S, typename... T>
	struct Exists
	: public FalseType { };
	
	template<typename S, typename... T>
	struct Exists<S, S, T...>
	: public TrueType { };
	
	template<typename S, typename H, typename... T>
	struct Exists<S, H, T...>
	: public Exists<S, T...> { };

	// get the index of S in the argument pack T
	template<typename, typename S, typename... T>
	struct IndexOfHelper { };
	
	template<typename S, typename... T>
	struct IndexOfHelper<EnableIfT<Exists<S, S, T...>::value>, S, S, T...>
	: public IntegralConstant<int, 0> { };
	
	template<typename S, typename H, typename... T>
	struct IndexOfHelper<EnableIfT<Exists<S, H, T...>::value>, S, H, T...>
	: public IntegralConstant<int, IndexOfHelper<void, S, T...>::value + 1> { };
	
	template<typename S, typename... T>
	using IndexOf = IndexOfHelper<void, S, T...>;

	// get a type with a certain index from the argument pack T
	template<int index, typename... T>
	struct GetHelper { };
	
	template<typename H, typename... T>
	struct GetHelper<0, H, T...> {
		using Type = H;
	};
	
	template<int index, typename H, typename... T>
	struct GetHelper<index, H, T...>
	: public GetHelper<index - 1, T...> { };
	
	template<int index, typename... T>
	using Get = typename GetHelper<index, T...>::Type;
};

template<typename... T>
struct Variant {
	template<typename X, int index = _variant::IndexOf<X, T...>::value>
	static constexpr int tagOf() {
		return index;
	}

	Variant()
	: _tag(-1) { }

	template<typename X, int index = _variant::IndexOf<X, T...>::value>
	Variant(X object)
	: Variant() {
		_construct(IntegralConstant<int, index>(), move(object));
	};

	Variant(const Variant &other)
	: Variant() {
		if(other)
			_copyConstruct(IntegralConstant<int, 0>(), other);
	}
	Variant(Variant &&other)
	: Variant() {
		if(other)
			_moveConstruct(IntegralConstant<int, 0>(), move(other));
			
	}

	~Variant() {
		if(*this)
			_destruct(IntegralConstant<int, 0>());
	}

	explicit operator bool() const {
		return _tag != -1;
	}

	Variant &operator= (Variant other) {
		// Because swap is quite hard to implement for this type we don't use copy-and-swap.
		// Instead we perform a destruct-then-move-construct operation on the internal object.
		// Note that we take the argument by value so there are no self-assignment problems.
		if(_tag == other._tag) {
			_assign(IntegralConstant<int, 0>(), move(other));
		}else{
			if(*this)
				_destruct(IntegralConstant<int, 0>());
			if(other)
				_moveConstruct(IntegralConstant<int, 0>(), move(other));
		}
		return *this;
	}

	int tag() {
		return _tag;
	}

	template<typename X, int index = _variant::IndexOf<X, T...>::value>
	bool is() const {
		return _tag == index;
	}

	template<typename X, int index = _variant::IndexOf<X, T...>::value>
	X &get() {
		assert(_tag == index);
		return *reinterpret_cast<X *>(_access());
	}
	template<typename X, int index = _variant::IndexOf<X, T...>::value>
	const X &get() const {
		assert(_tag == index);
		return *reinterpret_cast<const X *>(_access());
	}

	template<typename F>
	CommonType<ResultOf<F(T &)>...> apply(F functor) {
		return _apply(IntegralConstant<int, 0>(), move(functor));
	}

	template<typename F>
	CommonType<ResultOf<F(const T &)>...> const_apply(F functor) const {
		return _apply(IntegralConstant<int, 0>(), move(functor));
	}

private:
	void *_access() {
		return _storage.buffer;
	}
	const void *_access() const {
		return _storage.buffer;
	}

	// construct the internal object from one of the summed types
	template<int index, typename X = _variant::Get<index, T...>>
	void _construct(IntegralConstant<int, index>, X object) {
		assert(!*this);
		new (_access()) X(move(object));
		_tag = index;
	}

	// construct the internal object by copying from another variant
	template<int index, typename X = _variant::Get<index, T...>>
	void _copyConstruct(IntegralConstant<int, index>, const Variant &other) {
		if(other._tag == index) {
			assert(!*this);
			new (_access()) X(other.get<X>());
			_tag = index;
		}else{
			_copyConstruct(IntegralConstant<int, index + 1>(), other);
		}
	}

	void _copyConstruct(IntegralConstant<int, sizeof...(T)>, const Variant &) {
		assert(!"Copy-construction from variant with illegal tag");
	}

	// construct the internal object by moving from another variant
	template<int index, typename X = _variant::Get<index, T...>>
	void _moveConstruct(IntegralConstant<int, index>, Variant &&other) {
		if(other._tag == index) {
			assert(!*this);
			new (_access()) X(move(other.get<X>()));
			_tag = index;
		}else{
			_moveConstruct(IntegralConstant<int, index + 1>(), move(other));
		}
	}

	void _moveConstruct(IntegralConstant<int, sizeof...(T)>, Variant &&) {
		assert(!"Move-construction from variant with illegal tag");
	}
	
	// destruct the internal object
	template<int index, typename X = _variant::Get<index, T...>>
	void _destruct(IntegralConstant<int, index>) {
		if(_tag == index) {
			get<X>().~X();
			_tag = -1;
		}else{
			_destruct(IntegralConstant<int, index + 1>());
		}
	}
	
	void _destruct(IntegralConstant<int, sizeof...(T)>) {
		assert(!"Destruction of variant with illegal tag");
	}
	
	// assign the internal object
	template<int index, typename X = _variant::Get<index, T...>>
	void _assign(IntegralConstant<int, index>, Variant other) {
		if(_tag == index) {
			get<X>() = move(other.get<X>());
		}else{
			_assign(IntegralConstant<int, index +1>(), move(other));
		}
	}

	void _assign(IntegralConstant<int, sizeof...(T)>, Variant) {
		assert(!"Assignment from variant with illegal tag");
	}
	
	// apply a functor to the internal object
	template<typename F, int index, typename X = _variant::Get<index, T...>>
	CommonType<ResultOf<F(T &)>...>
	_apply(IntegralConstant<int, index>, F functor) {
		if(_tag == index) {
			return functor(get<X>());
		}else{
			return _apply(IntegralConstant<int, index + 1>(), move(functor));
		}
	}

	template<typename F>
	CommonType<ResultOf<F(T &)>...>
	_apply(IntegralConstant<int, sizeof...(T)>, F) {
		assert(!"_apply() on variant with illegal tag");
		__builtin_unreachable();
	}

	template<typename F, int index, typename X = _variant::Get<index, T...>>
	CommonType<ResultOf<F(T &)>...>
	const_apply(IntegralConstant<int, index>, F functor) const {
		if(_tag == index) {
			return functor(get<X>());
		}else{
			return _apply(IntegralConstant<int, index + 1>(), move(functor));
		}
	}

	template<typename F>
	CommonType<ResultOf<F(const T &)>...>
	const_apply(IntegralConstant<int, sizeof...(T)>, F) const {
		assert(!"_apply() on variant with illegal tag");
		__builtin_unreachable();
	}

	int _tag;
	AlignedUnion<T...> _storage;
};

} // namespace frigg

#endif // FRIGG_VARIANT_HPP

