
#ifndef FRIGG_INITIALIZER_HPP
#define FRIGG_INITIALIZER_HPP

#include <frigg/macros.hpp>
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>

namespace frigg FRIGG_VISIBILITY {

// note: this class has to be placed in zero'd memory
// (e.g. in the BSS segment), otherise _initialized will contain garbage
// we cannot use a ctor to initialize that field as this code is
// used in the dynamic linker and we want to avoid run-time relocations there
template<typename T>
class LazyInitializer {
public:
	template<typename... Args>
	void initialize(Args&&... args) {
		assert(!_initialized);
		new(&_storage) T(forward<Args>(args)...);
		_initialized = true;
	}

	void discard() {
		_initialized = false;
	}

	T *get() {
		assert(_initialized);
		return reinterpret_cast<T *>(&_storage);
	}
	T* unsafeGet() {
		return reinterpret_cast<T *>(&_storage);
	}

	operator bool () {
		return _initialized;
	}

	T *operator-> () {
		return get();
	}
	T &operator* () {
		return *get();
	}

private:
	AlignedStorage<sizeof(T), alignof(T)> _storage;
	bool _initialized;
};

// container for an object that 
template<typename T>
class Eternal {
public:
	static_assert(__has_trivial_destructor(AlignedStorage<sizeof(T), alignof(T)>),
			"Eternal<T> should have a trivial destructor");

	template<typename... Args>
	Eternal(Args &&... args) {
		new (&_storage) T(forward<Args>(args)...);
	}

	T &get() {
		return *reinterpret_cast<T *>(&_storage);
	}

private:
	AlignedStorage<sizeof(T), alignof(T)> _storage;
};

} // namespace frigg

#endif // FRIGG_INITIALIZER_HPP

