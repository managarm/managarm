
namespace thor {
namespace util {

namespace variant_impl {

// --------------------------------------------------------
// Helper classes
// --------------------------------------------------------

template<typename L, typename R>
struct TypesEq {
	static constexpr bool value = false;
};

template<typename T>
struct TypesEq<T, T> {
	static constexpr bool value = true;
};

template<typename T>
struct TryToInstantiate {
	static constexpr bool value = true;
};

// --------------------------------------------------------
// Storage
// --------------------------------------------------------

template<typename... Types>
union Storage;

template<typename T, typename... Tail>
union Storage<T, Tail...> {
	T element;
	Storage<Tail...> others;

	Storage() { }
};

template<>
union Storage<> { };

// --------------------------------------------------------
// Tag management
// --------------------------------------------------------

template<typename R, typename... Types>
struct TagOf;

template<typename R, bool found, typename T, typename... Tail>
struct TagOfStaticBranch;

template<typename R, typename T, typename... Tail>
struct TagOf<R, T, Tail...> {
	static constexpr int value
			= TagOfStaticBranch<R, TypesEq<R, T>::value, T, Tail...>::value;
};

template<typename R>
struct TagOf<R> {
	static_assert(!TryToInstantiate<R>::value, "Incompatible variant type");
};

template<typename R, typename T, typename... Tail>
struct TagOfStaticBranch<R, false, T, Tail...> {
	static constexpr int value = TagOf<R, Tail...>::value + 1;
};

template<typename R, typename T, typename... Tail>
struct TagOfStaticBranch<R, true, T, Tail...> {
	static constexpr int value = 1;
};

// --------------------------------------------------------
// Retrieval
// --------------------------------------------------------

template<typename R, typename... LookupTypes>
struct GetByType;

template<typename R, bool found, typename T, typename... Tail>
struct GetByTypeStaticBranch;

template<typename R, typename T, typename... Tail>
struct GetByType<R, T, Tail...> {
	static R &get(Storage<T, Tail...> &storage) {
		return GetByTypeStaticBranch<R, TypesEq<R, T>::value, T, Tail...>::get(storage);
	}
};

template<typename R>
struct GetByType<R> {
	static_assert(!TryToInstantiate<R>::value, "Incompatible variant type");
};

template<typename R, typename T, typename... Tail>
struct GetByTypeStaticBranch<R, false, T, Tail...> {
	static R &get(Storage<T, Tail...> &storage) {
		return GetByType<R, Tail...>::get(storage.others);
	}
};

template<typename R, typename T, typename... Tail>
struct GetByTypeStaticBranch<R, true, T, Tail...> {
	static R &get(Storage<T, Tail...> &storage) {
		return storage.element;
	}
};

// --------------------------------------------------------
// Destruction
// --------------------------------------------------------

template<int tag_iter, typename... Types>
struct Destruct;

template<int tag_iter, typename T, typename... Tail>
struct Destruct<tag_iter, T, Tail...> {
	static void destruct(int tag, Storage<T, Tail...> &storage) {
		if(tag == tag_iter) {
			storage.element.~T();
		}else{
			Destruct<tag_iter + 1, Tail...>::destruct(tag, storage.others);
		}
	}
};

template<int tag_iter>
struct Destruct<tag_iter> {
	static void destruct(int tag, Storage<> &storage) {
		ASSERT(!"Destruct: Illegal variant tag");
	}
};

// --------------------------------------------------------
// Copy construction and assignment
// --------------------------------------------------------

template<int tag_iter, typename... Types>
struct CopyConstruct;

template<int tag_iter, typename T, typename... Tail>
struct CopyConstruct<tag_iter, T, Tail...> {
	static void construct(int tag, Storage<T, Tail...> &dest, const Storage<T, Tail...> &src) {
		if(tag == tag_iter) {
			new (&dest) T(src.element);
		}else{
			CopyConstruct<tag_iter + 1, Tail...>::construct(tag, dest.others, src.others);
		}
	}
};

template<int tag_iter>
struct CopyConstruct<tag_iter> {
	static void construct(int tag, Storage<> &dest, const Storage<> &src) {
		ASSERT(!"CopyConstruct: Illegal variant tag");
	}
};

template<int tag_iter, typename... Types>
struct CopyAssign;

template<int tag_iter, typename T, typename... Tail>
struct CopyAssign<tag_iter, T, Tail...> {
	static void assign(int tag, Storage<T, Tail...> &dest, const Storage<T, Tail...> &src) {
		if(tag == tag_iter) {
			dest.element = src.element;
		}else{
			CopyAssign<tag_iter + 1, Tail...>::assign(tag, dest.others, src.others);
		}
	}
};

template<int tag_iter>
struct CopyAssign<tag_iter> {
	static void assign(int tag, Storage<> &dest, const Storage<> &src) {
		ASSERT(!"CopyAssign: Illegal variant tag");
	}
};

// --------------------------------------------------------
// Move construction and assignment
// --------------------------------------------------------

template<int tag_iter, typename... Types>
struct MoveConstruct;

template<int tag_iter, typename T, typename... Tail>
struct MoveConstruct<tag_iter, T, Tail...> {
	static void construct(int tag, Storage<T, Tail...> &dest, Storage<T, Tail...> &src) {
		if(tag == tag_iter) {
			new (&dest) T(util::move(src.element));
		}else{
			MoveConstruct<tag_iter + 1, Tail...>::construct(tag, dest.others, src.others);
		}
	}
};

template<int tag_iter>
struct MoveConstruct<tag_iter> {
	static void construct(int tag, Storage<> &dest, Storage<> &src) {
		ASSERT(!"MoveConstruct: Illegal variant tag");
	}
};

template<int tag_iter, typename... Types>
struct MoveAssign;

template<int tag_iter, typename T, typename... Tail>
struct MoveAssign<tag_iter, T, Tail...> {
	static void assign(int tag, Storage<T, Tail...> &dest, Storage<T, Tail...> &src) {
		if(tag == tag_iter) {
			dest.element = util::move(src.element);
		}else{
			MoveAssign<tag_iter + 1, Tail...>::assign(tag, dest.others, src.others);
		}
	}
};

template<int tag_iter>
struct MoveAssign<tag_iter> {
	static void assign(int tag, Storage<> &dest, Storage<> &src) {
		ASSERT(!"MoveAssign: Illegal variant tag");
	}
};

} // namespace variant_impl

// --------------------------------------------------------
// Variant class
// --------------------------------------------------------

template<typename... Types>
class Variant {
public:
	Variant() : p_tag(0) { }

	template<typename T>
	Variant(const T &element) {
		p_tag = variant_impl::TagOf<T, Types...>::value;
		new (&p_storage) T(element);
	}
	template<typename T>
	Variant(T &&element) {
		p_tag = variant_impl::TagOf<T, Types...>::value;
		new (&p_storage) T(util::move(element));
	}

	Variant(const Variant &other) {
		variant_impl::CopyConstruct<1, Types...>::construct(other.p_tag,
				p_storage, other.p_storage);
		p_tag = other.p_tag;
	}
	Variant(Variant &&other) {
		variant_impl::MoveConstruct<1, Types...>::construct(other.p_tag,
				p_storage, other.p_storage);
		p_tag = other.p_tag;
	}

	~Variant() {
		if(p_tag != 0)
			variant_impl::Destruct<1, Types...>::destruct(p_tag, p_storage);
	}

	Variant &operator= (const Variant &other) {
		if(other.p_tag == 0) {
			reset();
		}else if(p_tag == other.p_tag) {
			variant_impl::CopyAssign<1, Types...>::assign(p_tag, p_storage, other.p_storage);
		}else{
			reset();
			variant_impl::CopyConstruct<1, Types...>::construct(other.p_tag,
					p_storage, other.p_storage);
			p_tag = other.p_tag;
		}
		return *this;
	}
	Variant &operator= (Variant &&other) {
		if(other.p_tag == 0) {
			reset();
		}else if(p_tag == other.p_tag) {
			variant_impl::MoveAssign<1, Types...>::assign(p_tag, p_storage, other.p_storage);
		}else{
			reset();
			variant_impl::MoveConstruct<1, Types...>::construct(other.p_tag,
					p_storage, other.p_storage);
			p_tag = other.p_tag;
		}
		return *this;
	}

	template<typename T>
	static constexpr int tagOf() {
		return variant_impl::TagOf<T, Types...>::value;
	}

	void reset() {
		if(p_tag != 0)
			variant_impl::Destruct<1, Types...>::destruct(p_tag, p_storage);
		p_tag = 0;
	}

	bool empty() {
		return p_tag == 0;
	}

	int tag() {
		return p_tag;
	}
	
	template<typename T>
	bool is() {
		return p_tag == variant_impl::TagOf<T, Types...>::value;
	}

	template<typename T>
	T &get() {
		return variant_impl::GetByType<T, Types...>::get(p_storage);
	}

private:
	variant_impl::Storage<Types...> p_storage;
	int p_tag;
};

} } // namespace thor::util


