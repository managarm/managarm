
#ifndef FRIGG_SMART_PTR_HPP
#define FRIGG_SMART_PTR_HPP

#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>

namespace frigg {

// --------------------------------------------------------
// SharedPtr
// --------------------------------------------------------

template<typename T>
class SharedPtr;

template<typename T>
class WeakPtr;

template<typename T>
class UnsafePtr;

namespace _shared_ptr {
	struct Control {
		typedef void (*ControlFunction) (Control *);

		Control(ControlFunction destruct_me, ControlFunction free_me)
		: refCount(1), weakCount(1),
				destructMe(destruct_me), freeMe(free_me) { }

		Control(const Control &other) = delete;

		Control &operator= (const Control &other) = delete;

		~Control() {
			assert(volatileRead<int>(&refCount) == 0);
			assert(volatileRead<int>(&weakCount) == 0);
		}

		int refCount;
		int weakCount;
		ControlFunction destructMe;
		ControlFunction freeMe;
	};

	template<typename T>
	union Storage {
		Storage() { }
		~Storage() { }

		template<typename... Args>
		void construct(Args &&... args) {
			new (&object) T(forward<Args>(args)...);
		}
		
		void destruct() {
			object.~T();
		}

		T &operator* () {
			return object;
		}
		
		T object;
	};

};

template<typename T, typename Allocator>
struct SharedStruct : public _shared_ptr::Control {
	static void destructMe(_shared_ptr::Control *control) {
		auto *self = reinterpret_cast<SharedStruct *>(control);
		self->storage.destruct();
	}

	static void freeMe(_shared_ptr::Control *control) {
		auto *self = reinterpret_cast<SharedStruct *>(control);
		destruct(self->allocator, self);
	}

	template<typename... Args>
	SharedStruct(Allocator &allocator, Args &&... args)
	: _shared_ptr::Control(&destructMe, &freeMe), allocator(allocator) {
		storage.construct(forward<Args>(args)...);
	}

	_shared_ptr::Storage<T> storage;
	Allocator &allocator;
};

struct AdoptShared { };

static constexpr AdoptShared adoptShared;

template<typename T>
class SharedPtr {
	template<typename U>
	friend class SharedPtr;

	friend class UnsafePtr<T>;

public:
	friend void swap(SharedPtr &a, SharedPtr &b) {
		swap(a._control, b._control);
		swap(a._object, b._object);
	}

	SharedPtr()
	: _control(nullptr), _object(nullptr) { }
	
	template<typename Allocator>
	SharedPtr(AdoptShared, SharedStruct<T, Allocator> *block)
	: _control(block), _object(&(*block->storage)) {
		assert(block);
		assert(_control->refCount == 1);
		assert(_control->weakCount == 1);
	}

	SharedPtr(const SharedPtr &other)
	: _control(other._control), _object(other._object) {
		if(_control) {
			int previous_ref_count;
			fetchInc(&_control->refCount, previous_ref_count);
			assert(previous_ref_count > 0);
		}
	}

	SharedPtr(SharedPtr &&other)
	: SharedPtr() {
		swap(*this, other);
	}
	
	template<typename U>
	explicit SharedPtr(SharedPtr<U> pointer, T *alias)
	: _control(pointer._control), _object(alias) {
		// manually empty the argument pointer so that
		// its destructor does not decrement the reference count
		pointer._control = nullptr;
		pointer._object = nullptr;
	}

	explicit SharedPtr(const WeakPtr<T> &weak);
	
	explicit SharedPtr(const UnsafePtr<T> &unsafe);
	
	~SharedPtr() {
		if(_control) {
			int previous_ref_count;
			fetchDec(&_control->refCount, previous_ref_count);
			if(previous_ref_count == 1) {
				_control->destructMe(_control);

				int previous_weak_count;
				fetchDec(&_control->weakCount, previous_weak_count);
				assert(previous_weak_count > 0);
				if(previous_weak_count == 1)
					_control->freeMe(_control);
			}
		}
	}

	SharedPtr &operator= (SharedPtr other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return _control;
	}

	T *get() const {
		assert(_control);
		return _object;
	}
	T &operator* () const {
		assert(_control);
		return *_object;
	}
	T *operator-> () const {
		assert(_control);
		return _object;
	}

private:
	_shared_ptr::Control *_control;
	T *_object;
};
	
template<typename T, typename U>
SharedPtr<T> staticPtrCast(SharedPtr<U> pointer) {
	auto object = static_cast<T *>(pointer.get());
	return SharedPtr<T>(move(pointer), object);
}

template<typename T>
class WeakPtr {
	friend class SharedPtr<T>;
	friend class UnsafePtr<T>;
public:
	friend void swap(WeakPtr &a, WeakPtr &b) {
		swap(a._control, b._control);
		swap(a._object, b._object);
	}

	WeakPtr()
	: _control(nullptr), _object(nullptr) { }
	
	~WeakPtr() {
		if(_control) {
			int previous_weak_count;
			fetchDec(&_control->weakCount, previous_weak_count);
			assert(previous_weak_count > 0);
			if(previous_weak_count == 1)
				_control->freeMe(_control);
		}
	}

	WeakPtr(const WeakPtr &other)
	: _control(other._control), _object(other._object) {
		if(_control) {
			int previous_weak_count;
			fetchInc(&_control->weakCount, previous_weak_count);
			assert(previous_weak_count > 0);
		}
	}

	WeakPtr(WeakPtr &&other)
	: WeakPtr() {
		swap(*this, other);
	}
	
	explicit WeakPtr(const UnsafePtr<T> &unsafe);

	WeakPtr &operator= (WeakPtr other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return _control;
	}

private:
	_shared_ptr::Control *_control;
	T *_object;
};

template<typename T>
class UnsafePtr {
	friend class WeakPtr<T>;
	friend class SharedPtr<T>;
public:
	UnsafePtr()
	: _control(nullptr), _object(nullptr) { }
	
	UnsafePtr(const SharedPtr<T> &shared)
	: _control(shared._control), _object(shared._object) { }
	
	UnsafePtr(const WeakPtr<T> &weak)
	: _control(weak._control), _object(weak._object) { }

	template<typename U>
	explicit UnsafePtr(UnsafePtr<U> pointer, T *object)
	: _control(pointer._control), _object(object) { }

	explicit operator bool () {
		return _control;
	}

	T &operator* () {
		assert(_control);
		return *_object;
	}
	T *operator-> () {
		assert(_control);
		return _object;
	}
	T *get() const {
		assert(_control);
		return _object;
	}

private:
	_shared_ptr::Control *_control;
	T *_object;
};
	
template<typename T, typename U>
UnsafePtr<T> staticPtrCast(UnsafePtr<U> pointer) {
	auto object = static_cast<T *>(pointer.get());
	return UnsafePtr<T>(pointer, object);
}

template<typename T>
SharedPtr<T>::SharedPtr(const WeakPtr<T> &weak)
: _control(weak._control), _object(weak._object) {
	assert(_control);
	
	int last_count = volatileRead<int>(&_control->refCount);
	while(true) {
		if(last_count == 0) {
			_control = nullptr;
			_object = nullptr;
			break;
		}

		int found_count;
		if(compareSwap(&_control->refCount, last_count, last_count + 1, found_count))
			break;
		last_count = found_count;
	}
}

template<typename T>
SharedPtr<T>::SharedPtr(const UnsafePtr<T> &unsafe)
: _control(unsafe._control), _object(unsafe._object) {
	assert(_control);
	
	int previous_ref_count;
	fetchInc<int>(&_control->refCount, previous_ref_count);
	assert(previous_ref_count > 0);
}

template<typename T>
WeakPtr<T>::WeakPtr(const UnsafePtr<T> &unsafe)
: _control(unsafe._control), _object(unsafe._object) {
	assert(_control);

	int previous_weak_count;
	fetchInc<int>(&_control->weakCount, previous_weak_count);
	assert(previous_weak_count > 0);
}

template<typename T, typename Allocator, typename... Args>
SharedPtr<T> makeShared(Allocator &allocator, Args &&... args) {
	auto block = construct<SharedStruct<T, Allocator>>(allocator,
			allocator, forward<Args>(args)...);
	return SharedPtr<T>(adoptShared, block);

}

// --------------------------------------------------------
// UniqueMemory
// --------------------------------------------------------

template<typename Allocator>
class UniqueMemory {
public:
	friend void swap(UniqueMemory &a, UniqueMemory &b) {
		swap(a._pointer, b._pointer);
		swap(a._size, b._size);
		swap(a._allocator, b._allocator);
	}

	UniqueMemory()
	: _pointer(nullptr), _size(0), _allocator(nullptr) { }

	explicit UniqueMemory(Allocator &allocator, size_t size)
	: _size(size), _allocator(&allocator) {
		_pointer = _allocator->allocate(size);
	}

	UniqueMemory(UniqueMemory &&other)
	: UniqueMemory() {
		swap(*this, other);
	}

	UniqueMemory(const UniqueMemory &other) = delete;

	~UniqueMemory() {
		if(_pointer)
			_allocator->free(_pointer);
	}

	UniqueMemory &operator= (UniqueMemory other) {
		swap(*this, other);
		return *this;
	}

	void *data() {
		return _pointer;
	}

	size_t size() {
		return _size;
	}

private:
	void *_pointer;
	size_t _size;
	Allocator *_allocator;
};

} // namespace frigg

#endif // FRIGG_SMART_PTR_HPP

