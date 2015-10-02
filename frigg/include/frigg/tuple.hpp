
#ifndef FRIGG_TUPLE_HPP
#define FRIGG_TUPLE_HPP

namespace frigg {

namespace tuple_impl {

template<typename... Types>
struct Storage;

template<typename T, typename... Types>
struct Storage<T, Types...> {
	template<typename FwT, typename... FwTypes>
	Storage(FwT &&item, FwTypes &&... tail)
	: item(forward<FwT>(item)), tail(forward<FwTypes>(tail)...) { }

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

} // namespace tuple_impl

template<typename... Types>
class Tuple {
public:
	Tuple(const Tuple &other) = default;

	template<typename... FwTypes>
	explicit Tuple(FwTypes &&... args)
	: p_storage(forward<FwTypes>(args)...) { }

	template<int n>
	typename tuple_impl::NthType<n, Types...>::type &get() {
		return tuple_impl::Access<n, Types...>::access(p_storage);
	}

private:
	tuple_impl::Storage<Types...> p_storage;
};

template<typename... Types>
Tuple<typename RemoveRef<Types>::type...> makeTuple(Types &&... args) {
	return Tuple<typename RemoveRef<Types>::type...>(forward<Types>(args)...);
}

} // namespace frigg

#endif // FRIGG_TUPLE_HPP

