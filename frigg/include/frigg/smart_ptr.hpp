
#ifndef FRIGG_SMART_PTR_HPP
#define FRIGG_SMART_PTR_HPP

#include <frigg/macros.hpp>
#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>

namespace frigg FRIGG_VISIBILITY {

// --------------------------------------------------------
// SharedPtr
// --------------------------------------------------------

namespace _shared_ptr {
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

struct SharedCounter {
	enum Action {
		kCrtDestruct,
		kCrtFree
	};

	typedef void (*ControlFunction) (SharedCounter *, Action);

	SharedCounter(ControlFunction function, int ref_count = 1, int weak_count = 1)
	: _refCount(ref_count), _weakCount(weak_count), function(function) { }

	SharedCounter(const SharedCounter &other) = delete;

	~SharedCounter() {
		assert(volatileRead<int>(&_refCount) == 0);
		assert(volatileRead<int>(&_weakCount) == 0);
	}

	SharedCounter &operator= (const SharedCounter &other) = delete;

	// this function does NOT guarantee atomicity!
	// it is intended to be used during initialization when no
	// such guarantees are necessary.
	void setRelaxed(int value) {
		volatileWrite<int>(&_refCount, value);
	}

	// the following two operations are required for SharedPtrs
	void increment() {
		int previous_ref_count;
		fetchInc(&_refCount, previous_ref_count);
		assert(previous_ref_count > 0);
	}

	void decrement() {
		int previous_ref_count;
		fetchDec(&_refCount, previous_ref_count);
		if(previous_ref_count == 1) {
			decrementWeak();
			function(this, kCrtDestruct);
		}
	}

	// the following three operations are required fro WeakPtrs
	void incrementWeak() {
		int previous_weak_count;
		fetchInc<int>(&_weakCount, previous_weak_count);
		assert(previous_weak_count > 0);
	}

	void decrementWeak() {
		int previous_weak_count;
		fetchDec(&_weakCount, previous_weak_count);
		assert(previous_weak_count > 0);
		if(previous_weak_count == 1)
			function(this, kCrtFree);
	}

	bool tryToIncrement() {
		int last_count = volatileRead<int>(&_refCount);
		while(last_count) {
			if(compareSwap(&_refCount, last_count, last_count + 1, last_count))
				return true;
		}
		return false;
	}

private:
	int _refCount;
	int _weakCount;
	ControlFunction function;
};

struct SharedControl {
	SharedControl()
	: _counter(nullptr) { }

	SharedControl(SharedCounter *counter)
	: _counter(counter) { }

	explicit operator bool() const {
		return _counter;
	}

	SharedCounter *counter() { return _counter; }

	void increment() { _counter->increment(); }
	void decrement() { _counter->decrement(); }
	
	void incrementWeak() { _counter->incrementWeak(); }
	void decrementWeak() { _counter->decrementWeak(); }
	bool tryToIncrement() { return _counter->tryToIncrement(); }

private:
	SharedCounter *_counter;
};

template<typename T, typename Control = SharedControl>
class SharedPtr;

template<typename T, typename Control = SharedControl>
class WeakPtr;

template<typename T, typename Control = SharedControl>
class UnsafePtr;

template<typename T, typename Allocator>
struct SharedBlock : public SharedCounter {
	static void doControl(SharedCounter *counter, SharedCounter::Action action) {
		auto self = static_cast<SharedBlock *>(counter);
		if(action == SharedCounter::kCrtDestruct) {
			self->_storage.destruct();
		}else{
			assert(action == SharedCounter::kCrtFree);
			destruct(self->_allocator, self);
		}
	}

	template<typename... Args>
	SharedBlock(Allocator &allocator, Args &&... args)
	: SharedCounter(&doControl), _allocator(allocator) {
		_storage.construct(forward<Args>(args)...);
	}

	T *get() {
		return &(*_storage);
	}

private:
	_shared_ptr::Storage<T> _storage;
	Allocator &_allocator;
};

struct AdoptShared { };

static constexpr AdoptShared adoptShared;

// NOTE: the memory layout of SharedPtr, WeakPtr and UnsafePtr is fixed!
// It may be accessed by assembly code; do not change the field offsets!
// Each of these structs consists of two pointers: the first one points
// to a opaque control structure while the second one points to the actual object.

template<typename T, typename Control>
class SharedPtr {
	template<typename U, typename D>
	friend class SharedPtr;

	friend class WeakPtr<T, Control>;
	friend class UnsafePtr<T, Control>;

public:
	friend void swap(SharedPtr &a, SharedPtr &b) {
		swap(a._control, b._control);
		swap(a._object, b._object);
	}

	SharedPtr()
	: _object(nullptr) { }
	
	template<typename Allocator>
	SharedPtr(AdoptShared, SharedBlock<T, Allocator> *block)
	: _control(block), _object(block->get()) {
		assert(_control);
	}
	
	SharedPtr(AdoptShared, T *object, Control control)
	: _control(control), _object(object) {
		assert(_control);
	}

	SharedPtr(const SharedPtr &other)
	: _control(other._control), _object(other._object) {
		if(_control)
			_control.increment();
	}

	SharedPtr(SharedPtr &&other)
	: SharedPtr() {
		swap(*this, other);
	}
	
	template<typename U>
	SharedPtr(SharedPtr<U, Control> pointer, T *alias)
	: _control(pointer._control), _object(alias) {
		// manually empty the argument pointer so that
		// its destructor does not decrement the reference count
		pointer._control = Control();
		pointer._object = nullptr;
	}
	
	template<typename U, typename = EnableIfT<IsConvertible<U *, T *>::value>>
	SharedPtr(SharedPtr<U, Control> pointer)
	: _control(pointer._control), _object(pointer._object) {
		// manually empty the argument pointer so that
		// its destructor does not decrement the reference count
		pointer._control = Control();
		pointer._object = nullptr;
	}
	
	template<typename D>
	SharedPtr(const SharedPtr<T, D> &other)
	: _control(other._control), _object(other._object) {
		if(_control)
			_control.increment();
	}

	~SharedPtr() {
		if(_control)
			_control.decrement();
	}

	SharedPtr &operator= (SharedPtr other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return (bool)_control;
	}

	Control control() const {
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

	void release() {
		_control = Control();
		_object = nullptr;
	}

private:
	SharedPtr(Control control, T *object)
	: _control(control), _object(object) { }

	Control _control;
	T *_object;
};
	
template<typename T, typename U>
SharedPtr<T> staticPtrCast(SharedPtr<U> pointer) {
	auto object = static_cast<T *>(pointer.get());
	return SharedPtr<T>(move(pointer), object);
}

template<typename T, typename Control>
class WeakPtr {
	friend class UnsafePtr<T, Control>;
public:
	friend void swap(WeakPtr &a, WeakPtr &b) {
		swap(a._control, b._control);
		swap(a._object, b._object);
	}

	WeakPtr()
	: _control(nullptr), _object(nullptr) { }
	
	WeakPtr(const SharedPtr<T, Control> &shared)
	: _control(shared._control), _object(shared._object) {
		assert(_control);
		_control.incrementWeak();
	}

	WeakPtr(const WeakPtr &other)
	: _control(other._control), _object(other._object) {
		if(_control)
			_control.incrementWeak();
	}

	WeakPtr(WeakPtr &&other)
	: WeakPtr() {
		swap(*this, other);
	}
	
	~WeakPtr() {
		if(_control)
			_control.decrementWeak();
	}
	
	SharedPtr<T, Control> grab() {
		assert(_control);
		
		if(_control.tryToIncrement())
				return SharedPtr<T, Control>(_control, _object);
		return SharedPtr<T, Control>();
	}

	WeakPtr &operator= (WeakPtr other) {
		swap(*this, other);
		return *this;
	}

	explicit operator bool () {
		return (bool)_control;
	}

private:
	WeakPtr(Control control, T *object)
	: _control(control), _object(object) { }

	Control _control;
	T *_object;
};

template<typename T, typename Control>
class UnsafePtr {
	template<typename U, typename D>
	friend class UnsafePtr;

public:
	UnsafePtr()
	: _control(nullptr), _object(nullptr) { }
	
	UnsafePtr(const SharedPtr<T, Control> &shared)
	: _control(shared._control), _object(shared._object) { }
	
	UnsafePtr(const WeakPtr<T, Control> &weak)
	: _control(weak._control), _object(weak._object) { }

	template<typename U>
	UnsafePtr(UnsafePtr<U, Control> pointer, T *object)
	: _control(pointer._control), _object(object) { }
	
	template<typename U, typename = EnableIfT<IsConvertible<U *, T *>::value>>
	UnsafePtr(UnsafePtr<U, Control> pointer)
	: _control(pointer._control), _object(pointer._object) { }

	SharedPtr<T, Control> toShared() {
		assert(_control);
		_control.increment();
		return SharedPtr<T, Control>(_control, _object);
	}

	WeakPtr<T, Control> toWeak() {
		assert(_control);
		_control.incrementWeak();
		return WeakPtr<T, Control>(_control, _object);
	}

	explicit operator bool () {
		return (bool)_control;
	}

	Control control() const {
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
	Control _control;
	T *_object;
};
	
template<typename T, typename U>
UnsafePtr<T> staticPtrCast(UnsafePtr<U> pointer) {
	auto object = static_cast<T *>(pointer.get());
	return UnsafePtr<T>(pointer, object);
}

template<typename T, typename Allocator, typename... Args>
SharedPtr<T> makeShared(Allocator &allocator, Args &&... args) {
	auto block = construct<SharedBlock<T, Allocator>>(allocator,
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

