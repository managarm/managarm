
#ifndef FRIGG_INITIALIZER_HPP
#define FRIGG_INITIALIZER_HPP

#include <frigg/macros.hpp>
#include <frigg/cxx-support.hpp>
#include <frigg/traits.hpp>

namespace frigg FRIGG_VISIBILITY {

// note: this class has to be placed in zero'd memory
// (e.g. in the BSS segment), otherise p_initialized will contain garbage
// we cannot use a ctor to initialize that field as this code is
// used in the dynamic linker and we want to avoid run-time relocations there
template<typename T>
class LazyInitializer {
public:
	template<typename... Args>
	void initialize(Args&&... args) {
		assert(!p_initialized);
		new(p_object) T(forward<Args>(args)...);
		p_initialized = true;
	}

	void discard() {
		p_initialized = false;
	}

	T *get() {
		assert(p_initialized);
		return reinterpret_cast<T *>(p_object);
	}
	T* unsafeGet() {
		return reinterpret_cast<T *>(p_object);
	}

	operator bool () {
		return p_initialized;
	}

	T *operator-> () {
		return get();
	}
	T &operator* () {
		return *get();
	}

private:
	char p_object[sizeof(T)];
	bool p_initialized;
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

