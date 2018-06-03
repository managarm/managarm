
#ifndef FRIGG_TUPLE_HPP
#define FRIGG_TUPLE_HPP

#include <frigg/macros.hpp> 
#include <frigg/traits.hpp> 

namespace frigg FRIGG_VISIBILITY {

namespace _tuple {
	template<typename... Types>
	struct Storage;

	template<typename T, typename... Types>
	struct Storage<T, Types...> {
		Storage() = default;

		Storage(T item, Types... tail)
		: item(move(item)), tail(move(tail)...) { }

		T item;
		Storage<Types...> tail;
	};

	template<>
	struct Storage<> {

	};

	template<int n, typename... Types>
	struct NthType;

	template<int n, typename T, typename... Types>
	struct NthType<n, T, Types...> {
		typedef typename NthType<n - 1, Types...>::type  type;
	};

	template<typename T, typename... Types>
	struct NthType<0, T, Types...> {
		typedef T type;
	};

	template<int n, typename... Types>
	struct Access;

	template<int n, typename T, typename... Types>
	struct Access<n, T, Types...> {
		static typename NthType<n - 1, Types...>::type &access(Storage<T, Types...> &storage) {
			return Access<n - 1, Types...>::access(storage.tail);
		}
	};

	template<typename T, typename... Types>
	struct Access<0, T, Types...> {
		static T &access(Storage<T, Types...> &storage) {
			return storage.item;
		}
	};
} // namespace _tuple

template<typename... Types>
class Tuple {
public:
	Tuple() = default;

	explicit Tuple(Types... args)
	: p_storage(move(args)...) { }

	template<int n>
	typename _tuple::NthType<n, Types...>::type &get() {
		return _tuple::Access<n, Types...>::access(p_storage);
	}

private:
	_tuple::Storage<Types...> p_storage;
};

// Specialization to allow empty tuples.
template<>
class Tuple<> { };

template<typename... Types>
Tuple<typename RemoveRef<Types>::type...> makeTuple(Types &&... args) {
	return Tuple<typename RemoveRef<Types>::type...>(forward<Types>(args)...);
}

namespace _tuple {
	template<typename F, typename... Args, size_t... I>
	auto applyToFunctor(F functor, Tuple<Args...> args, IndexSequence<I...>) {
		return functor(move(args.template get<I>())...);
	}
} // namespace tuple

template<typename F, typename... Args>
auto applyToFunctor(F functor, Tuple<Args...> args) {
	return _tuple::applyToFunctor(move(functor), move(args), IndexSequenceFor<Args...>());
}


} // namespace frigg

#endif // FRIGG_TUPLE_HPP

