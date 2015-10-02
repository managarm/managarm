
#ifndef FRIGG_OPTIONAL_HPP
#define FRIGG_OPTIONAL_HPP

namespace frigg {

namespace expected_impl {
	template<typename T>
	struct ExpectedConstructor {
		T expected;
		
		ExpectedConstructor(const T &expected)
		: expected(expected) { }

		ExpectedConstructor(T &&expected)
		: expected(move(expected)) { }
	};

	template<typename E>
	struct ErrorConstructor {
		E error;

		ErrorConstructor(const E &error)
		: error(error) { }

		ErrorConstructor(E &&error)
		: error(move(error)) { }
	};

} // namespace expected_impl

template<typename T, typename E>
class Expected {
public:
	Expected(expected_impl::ExpectedConstructor<T> &&constructor)
	: p_isExpected(true) {
		new (&p_union.expected) T(move(constructor.expected));
	}
	
	Expected(expected_impl::ErrorConstructor<E> &&constructor)
	: p_isExpected(false) {
		new (&p_union.error) E(move(constructor.error));
	}
	
	Expected(const Expected &other) : p_isExpected(other.p_isExpected) {
		if(p_isExpected) {
			new (&p_union.expected) T(other.p_union.expected);
		}else{
			new (&p_union.error) E(other.p_union.error);
		}
	}
	
	Expected(Expected &&other) : p_isExpected(other.p_isExpected) {
		if(p_isExpected) {
			new (&p_union.expected) T(move(other.p_union.expected));
		}else{
			new (&p_union.error) E(move(other.p_union.error));
		}
	}

	~Expected() {
		if(p_isExpected) {
			p_union.expected.~T();
		}else{
			p_union.error.~E();
		}
	}

	Expected &operator= (Expected copy) {
		swap(*this, copy);
		return *this;
	}
	
	operator bool () {
		return p_isExpected;
	}

	T &operator* () {
		assert(p_isExpected);
		return p_union.expected;
	}
	T *operator-> () {
		assert(p_isExpected);
		return &p_union.expected;
	}

	E &error() {
		return p_union.error;
	}

	friend void swap(Expected &first, Expected &second) {
		if(first.p_isExpected && second.p_isExpected) {
			swap(first.p_union.expected, second.p_union.expected);
		}else if(!first.p_isExpected && !second.p_isExpected) {
			swap(first.p_union.error, second.p_union.error);
		}else if(first.p_isExpected && !second.p_isExpected) {
			T first_expected(move(first.p_union.expected));
			E second_error(move(second.p_union.error));

			first.p_union.expected.~T();
			second.p_union.error.~E();

			new (&first.p_union.error) E(move(second_error));
			new (&second.p_union.expected) T(move(first_expected));
		}else{
			assert(!first.p_isExpected && second.p_isExpected);
			E first_error(move(first.p_union.error));
			T second_expected(move(second.p_union.expected));

			first.p_union.error.~E();
			second.p_union.expected.~T();

			new (&first.p_union.expected) T(move(second_expected));
			new (&second.p_union.error) E(move(first_error));
		}
		swap(first.p_isExpected, second.p_isExpected);
	}

private:
	union InternalUnion {
		T expected;
		E error;
		
		InternalUnion() { }
		~InternalUnion() { }
	};

	InternalUnion p_union;
	bool p_isExpected;
};

template<typename T>
expected_impl::ExpectedConstructor<typename RemoveRef<T>::type> expected(T &&expected) {
	return expected_impl::ExpectedConstructor<typename RemoveRef<T>::type>(forward<T>(expected));
}

template<typename E>
expected_impl::ErrorConstructor<typename RemoveRef<E>::type> error(E &&error) {
	return expected_impl::ErrorConstructor<typename RemoveRef<E>::type>(forward<E>(error));
}

template<typename T>
class Optional {
public:
	Optional() : p_hasOptional(false) { }

	Optional(const T &object) : p_hasOptional(true) {
		new (&p_union.object) T(object);
	}

	Optional(T &&object) : p_hasOptional(true) {
		new (&p_union.object) T(move(object));
	}

	Optional(const Optional &other) : p_hasOptional(other.p_hasOptional) {
		if(p_hasOptional)
			new (&p_union.object) T(other.p_union.object);
	}

	Optional(Optional &&other) : p_hasOptional(other.p_hasOptional) {
		if(p_hasOptional) {
			new (&p_union.object) T(move(other.p_union.object));
			other.p_hasOptional = false;
		}
	}

	~Optional() {
		if(p_hasOptional)
			p_union.object.~T();
	}

	Optional &operator= (Optional other) {
		swap(*this, other);
		return *this;
	}

	operator bool() {
		return p_hasOptional;
	}

	T &operator* () {
		assert(p_hasOptional);
		return p_union.object;
	}
	T *operator-> () {
		assert(p_hasOptional);
		return &p_union.object;
	}

	friend void swap(Optional &first, Optional &second) {
		if(first.p_hasOptional && second.p_hasOptional) {
			swap(first.p_union.object, second.p_union.object);
		}else if(first.p_hasOptional && !second.p_hasOptional) {
			T first_object(move(first.p_union.object));

			first.p_union.object.~T();
			new (&second.p_union.object) T(move(first_object));
		}else if(!first.p_hasOptional && second.p_hasOptional) {
			T second_object(move(second.p_union.object));

			second.p_union.object.~T();
			new (&first.p_union.object) T(move(second_object));
		}
		swap(first.p_hasOptional, second.p_hasOptional);
	}
private:
	union InternalUnion {
		T object;
		
		InternalUnion() { }
		~InternalUnion() { } // handled by super class destructor
	};

	InternalUnion p_union;
	bool p_hasOptional;
};

} // namespace frigg

#endif // FRIGG_OPTIONAL_HPP

