
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

struct SharedBlock {
	typedef void (*DeleteFuncPtr) (SharedBlock *block);

	SharedBlock(DeleteFuncPtr destruct_function, DeleteFuncPtr free_function)
	: refCount(1), weakCount(1),
			destructFunction(destruct_function), freeFunction(free_function) { }

	SharedBlock(const SharedBlock &other) = delete;

	SharedBlock &operator= (const SharedBlock &other) = delete;

	~SharedBlock() {
		assert(volatileRead<int>(&refCount) == 0);
		assert(volatileRead<int>(&weakCount) == 0);
	}

	int refCount;
	int weakCount;
	DeleteFuncPtr destructFunction;
	DeleteFuncPtr freeFunction;
};

template<typename T>
union SharedStorage {
	SharedStorage() { }
	~SharedStorage() { }

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

template<typename T, typename Allocator>
struct SharedStruct : public SharedBlock {
	static void destructFunction(SharedBlock *block) {
		auto *self = reinterpret_cast<SharedStruct *>(block);
		self->storage.destruct();
	}

	static void freeFunction(SharedBlock *block) {
		auto *self = reinterpret_cast<SharedStruct *>(block);
		destruct(self->allocator, self);
	}

	template<typename... Args>
	SharedStruct(Allocator &allocator, Args &&... args)
	: SharedBlock(&destructFunction, &freeFunction), allocator(allocator) {
		storage.construct(forward<Args>(args)...);
	}

	SharedStorage<T> storage;
	Allocator &allocator;
};

namespace details {

struct Cast {
	template<typename U, typename T>
	static SharedPtr<U> staticPtrCast(SharedPtr<T> pointer) {
		SharedPtr<U> result(pointer.p_block, static_cast<U *>(pointer.p_object));
		// reset the input pointer to keep the ref count accurate
		pointer.p_block = nullptr;
		pointer.p_object = nullptr;
		return move(result);
	}
	
	template<typename U, typename T>
	static UnsafePtr<U> staticPtrCast(UnsafePtr<T> pointer) {
		return UnsafePtr<U>(pointer.p_block, static_cast<U *>(pointer.p_object));
	}
};

} // namespace details

template<typename T>
class SharedPtr {
	friend class UnsafePtr<T>;

	friend class details::Cast;

public:
	template<typename Allocator, typename... Args>
	static SharedPtr make(Allocator &allocator, Args &&... args) {
		auto shared_struct = construct<SharedStruct<T, Allocator>>
				(allocator, allocator, forward<Args>(args)...);
		return SharedPtr(static_cast<SharedBlock *>(shared_struct),
				&(*shared_struct->storage));
	}

	SharedPtr() : p_block(nullptr), p_object(nullptr) { }
	
	~SharedPtr() {
		reset();
	}

	SharedPtr(const SharedPtr &other) {
		p_block = other.p_block;
		p_object = other.p_object;
		if(p_block != nullptr) {
			int old_ref_count;
			fetchInc(&p_block->refCount, old_ref_count);
			assert(old_ref_count > 0);
		}
	}
	
	explicit SharedPtr(const WeakPtr<T> &weak);
	
	explicit SharedPtr(const UnsafePtr<T> &unsafe);

	SharedPtr(SharedPtr &&other) {
		p_block = other.p_block;
		p_object = other.p_object;
		other.p_block = nullptr;
		other.p_object = nullptr;
	}

	SharedPtr &operator= (SharedPtr &&other) {
		reset();
		p_block = other.p_block;
		p_object = other.p_object;
		other.p_block = nullptr;
		other.p_object = nullptr;
		return *this;
	}

	operator bool () {
		return p_block != nullptr;
	}

	void reset() {
		if(p_block == nullptr)
			return;

		int old_ref_count;
		fetchDec(&p_block->refCount, old_ref_count);
		if(old_ref_count == 1) {
			p_block->destructFunction(p_block);

			int old_weak_count;
			fetchDec(&p_block->weakCount, old_weak_count);
			assert(old_weak_count > 0);
			if(old_weak_count == 1)
				p_block->freeFunction(p_block);
		}
		p_block = nullptr;
		p_object = nullptr;
	}

	T *get() const {
		return p_object;
	}
	T &operator* () const {
		return *p_object;
	}
	T *operator-> () const {
		return p_object;
	}

private:
	SharedPtr(SharedBlock *block, T *object)
	: p_block(block), p_object(object) { }

	SharedBlock *p_block;
	T *p_object;
};
	
template<typename U, typename T>
SharedPtr<U> staticPtrCast(SharedPtr<T> pointer) {
	return details::Cast::staticPtrCast<U>(pointer);
}

template<typename T>
class WeakPtr {
	friend class SharedPtr<T>;
	friend class UnsafePtr<T>;
public:
	WeakPtr() : p_block(nullptr), p_object(nullptr) { }
	
	~WeakPtr() {
		reset();
	}

	WeakPtr(const WeakPtr &other) {
		p_block = other.p_block;
		p_object = other.p_object;
		if(p_block != nullptr) {
			int old_weak_count;
			fetchInc(&p_block->weakCount, old_weak_count);
			assert(old_weak_count > 0);
		}
	}
	
	explicit WeakPtr(const UnsafePtr<T> &unsafe);

	WeakPtr(WeakPtr &&other) {
		p_block = other.p_block;
		p_object = other.p_object;
		other.p_block = nullptr;
		other.p_object = nullptr;
	}

	WeakPtr &operator= (WeakPtr &&other) {
		reset();
		p_block = other.p_block;
		p_object = other.p_object;
		other.p_block = nullptr;
		other.p_object = nullptr;
		return *this;
	}

	operator bool () {
		return p_block != nullptr;
	}

	void reset() {
		if(p_block == nullptr)
			return;

		int old_weak_count;
		fetchDec(&p_block->weakCount, old_weak_count);
		assert(old_weak_count > 0);
		if(old_weak_count == 1)
			p_block->freeFunction(p_block);
		
		p_block = nullptr;
		p_object = nullptr;
	}

private:
	SharedBlock *p_block;
	T *p_object;
};

template<typename T>
class UnsafePtr {
	friend class WeakPtr<T>;
	friend class SharedPtr<T>;

	friend class details::Cast;
public:
	UnsafePtr() : p_block(nullptr), p_object(nullptr) { }
	
	UnsafePtr(const SharedPtr<T> &shared)
	: p_block(shared.p_block), p_object(shared.p_object) { }
	
	UnsafePtr(const WeakPtr<T> &weak)
	: p_block(weak.p_block), p_object(weak.p_object) { }

	operator bool () {
		return p_block != nullptr;
	}

	T *operator-> () {
		return p_object;
	}
	T *get() {
		return p_object;
	}

private:
	SharedBlock *p_block;
	T *p_object;
};
	
template<typename U, typename T>
UnsafePtr<U> staticPtrCast(UnsafePtr<T> pointer) {
	return details::Cast::staticPtrCast<U>(pointer);
}

template<typename T>
SharedPtr<T>::SharedPtr(const WeakPtr<T> &weak)
: p_block(weak.p_block), p_object(weak.p_object) {
	int last_count = volatileRead<int>(&p_block->refCount);
	while(true) {
		if(last_count == 0) {
			p_block = nullptr;
			p_object = nullptr;
			break;
		}

		int found_count;
		if(compareSwap(&p_block->refCount, last_count, last_count + 1, found_count))
			break;
		last_count = found_count;
	}
}

template<typename T>
SharedPtr<T>::SharedPtr(const UnsafePtr<T> &unsafe)
: p_block(unsafe.p_block), p_object(unsafe.p_object) {
	int old_ref_count;
	fetchInc<int>(&p_block->refCount, old_ref_count);
	assert(old_ref_count > 0);
}

template<typename T>
WeakPtr<T>::WeakPtr(const UnsafePtr<T> &unsafe)
: p_block(unsafe.p_block), p_object(unsafe.p_object) {
	int old_weak_count;
	fetchInc<int>(&p_block->weakCount, old_weak_count);
	assert(old_weak_count > 0);
}

template<typename T, typename Allocator, typename... Args>
SharedPtr<T> makeShared(Allocator &allocator, Args&&... args) {
	return SharedPtr<T>::make(allocator, forward<Args>(args)...);
}

// --------------------------------------------------------
// UniqueMemory
// --------------------------------------------------------

template<typename Allocator>
class UniqueMemory {
public:
	UniqueMemory()
	: p_pointer(nullptr), p_size(0), p_allocator(nullptr) { }

	explicit UniqueMemory(Allocator &allocator, size_t size)
	: p_size(size), p_allocator(&allocator) {
		p_pointer = p_allocator->allocate(size);
	}

	UniqueMemory(UniqueMemory &&other)
	: UniqueMemory() {
		swap(*this, other);
	}

	~UniqueMemory() {
		reset();
	}

	UniqueMemory(const UniqueMemory &other) = delete;

	UniqueMemory &operator= (UniqueMemory other) {
		swap(*this, other);
		return *this;
	}

	void reset() {
		if(p_pointer)
			p_allocator->free(p_pointer);
		p_pointer = nullptr;
		p_size = 0;
	}

	friend void swap(UniqueMemory &a, UniqueMemory &b) {
		swap(a.p_pointer, b.p_pointer);
		swap(a.p_size, b.p_size);
		swap(a.p_allocator, b.p_allocator);
	}

	void *data() {
		return p_pointer;
	}

	size_t size() {
		return p_size;
	}

private:
	void *p_pointer;
	size_t p_size;
	Allocator *p_allocator;
};

} // namespace frigg

#endif // FRIGG_SMART_PTR_HPP

