
namespace frigg {

template<typename T, typename Allocator>
class SharedPtr;

template<typename T, typename Allocator>
class WeakPtr;

template<typename T, typename Allocator>
class UnsafePtr;

template<typename Allocator>
struct SharedBlock {
	typedef void (*DeleteFuncPtr) (SharedBlock *block);

	SharedBlock(Allocator &allocator, DeleteFuncPtr delete_object)
	: allocator(allocator), refCount(1), weakCount(1), deleteObject(delete_object) { }

	SharedBlock(const SharedBlock &other) = delete;

	SharedBlock &operator= (const SharedBlock &other) = delete;

	~SharedBlock() {
		assert(volatileRead<int>(&refCount) == 0);
		assert(volatileRead<int>(&weakCount) == 0);
	}

	Allocator &allocator;
	int refCount;
	int weakCount;
	DeleteFuncPtr deleteObject;
};

template<typename T>
union SharedStorage {
	SharedStorage() { }
	~SharedStorage() { }

	template<typename... Args>
	void construct(Args &&... args) {
		new (&object) T(traits::forward<Args>(args)...);
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
struct SharedStruct {
	static void deleteObject(SharedBlock<Allocator> *block) {
		auto *self = reinterpret_cast<SharedStruct *>(block);
		self->storage.destruct();
	}

	template<typename... Args>
	SharedStruct(Allocator &allocator, Args &&... args)
	: block(allocator, &deleteObject) {
		storage.construct(traits::forward<Args>(args)...);
	}

	SharedBlock<Allocator> block;
	SharedStorage<T> storage;
};

namespace details {

struct Cast {
	template<typename U, typename T, typename Allocator>
	static SharedPtr<U, Allocator> staticPointerCast(SharedPtr<T, Allocator> pointer) {
		SharedPtr<U, Allocator> result(pointer.p_block, static_cast<U *>(pointer.p_object));
		// reset the input pointer to keep the ref count accurate
		pointer.p_block = nullptr;
		pointer.p_object = nullptr;
		return traits::move(result);
	}
	
	template<typename U, typename T, typename Allocator>
	static UnsafePtr<U, Allocator> staticPointerCast(UnsafePtr<T, Allocator> pointer) {
		return UnsafePtr<U, Allocator>(pointer.p_block, static_cast<U *>(pointer.p_object));
	}
};

} // namespace details

template<typename T, typename Allocator>
class SharedPtr {
	friend class UnsafePtr<T, Allocator>;

	friend class details::Cast;

public:
	template<typename... Args>
	static SharedPtr make(Allocator &allocator, Args &&... args) {
		auto shared_struct = memory::construct<SharedStruct<T, Allocator>>
				(allocator, allocator, traits::forward<Args>(args)...);
		return SharedPtr<T, Allocator>(reinterpret_cast<SharedBlock<Allocator> *>(shared_struct),
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
	
	explicit SharedPtr(const WeakPtr<T, Allocator> &weak);
	
	explicit SharedPtr(const UnsafePtr<T, Allocator> &unsafe);

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
			p_block->deleteObject(p_block);

			int old_weak_count;
			fetchDec(&p_block->weakCount, old_weak_count);
			assert(old_weak_count > 0);
			if(old_weak_count == 1)
				memory::destruct(p_block->allocator, p_block);
		}
		p_block = nullptr;
		p_object = nullptr;
	}

	T *operator-> () const {
		return p_object;
	}
	T *get() const {
		return p_object;
	}

private:
	SharedPtr(SharedBlock<Allocator> *block, T *object)
	: p_block(block), p_object(object) { }

	SharedBlock<Allocator> *p_block;
	T *p_object;
};
	
template<typename U, typename T, typename Allocator>
SharedPtr<U, Allocator> staticPointerCast(SharedPtr<T, Allocator> pointer) {
	return details::Cast::staticPointerCast<U>(pointer);
}

template<typename T, typename Allocator>
class WeakPtr {
	friend class SharedPtr<T, Allocator>;
	friend class UnsafePtr<T, Allocator>;
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
	
	explicit WeakPtr(const UnsafePtr<T, Allocator> &unsafe);

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
			memory::destruct(p_block->allocator, p_block);
		
		p_block = nullptr;
		p_object = nullptr;
	}

private:
	SharedBlock<Allocator> *p_block;
	T *p_object;
};

template<typename T, typename Allocator>
class UnsafePtr {
	friend class WeakPtr<T, Allocator>;
	friend class SharedPtr<T, Allocator>;

	friend class details::Cast;
public:
	UnsafePtr() : p_block(nullptr), p_object(nullptr) { }
	
	UnsafePtr(const SharedPtr<T, Allocator> &shared)
	: p_block(shared.p_block), p_object(shared.p_object) { }

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
	SharedBlock<Allocator> *p_block;
	T *p_object;
};
	
template<typename U, typename T, typename Allocator>
UnsafePtr<U, Allocator> staticPointerCast(UnsafePtr<T, Allocator> pointer) {
	return details::Cast::staticPointerCast<U>(pointer);
}

template<typename T, typename Allocator>
SharedPtr<T, Allocator>::SharedPtr(const WeakPtr<T, Allocator> &weak)
: p_block(weak.p_block), p_object(weak.p_object) {
	int last_ref_count = volatileRead<int>(&p_block->refCount);
	while(true) {
		if(last_ref_count == 0) {
			p_block = nullptr;
			p_object = nullptr;
			break;
		}

		int found_ref_count;
		if(compareSwap(&p_block->refCount,
				last_ref_count, last_ref_count + 1, found_ref_count))
			break;
		last_ref_count = found_ref_count;
	}
}

template<typename T, typename Allocator>
SharedPtr<T, Allocator>::SharedPtr(const UnsafePtr<T, Allocator> &unsafe)
: p_block(unsafe.p_block), p_object(unsafe.p_object) {
	int old_ref_count;
	fetchInc<int>(&p_block->refCount, old_ref_count);
	assert(old_ref_count > 0);
}

template<typename T, typename Allocator>
WeakPtr<T, Allocator>::WeakPtr(const UnsafePtr<T, Allocator> &unsafe)
: p_block(unsafe.p_block), p_object(unsafe.p_object) {
	int old_weak_count;
	fetchInc<int>(&p_block->weakCount, old_weak_count);
	assert(old_weak_count > 0);
}

template<typename T, typename Allocator, typename... Args>
SharedPtr<T, Allocator> makeShared(Allocator &allocator, Args&&... args) {
	return SharedPtr<T, Allocator>::make(allocator, traits::forward<Args>(args)...);
}

} // namespace frigg

