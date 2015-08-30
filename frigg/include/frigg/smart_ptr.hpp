
namespace frigg {

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
class SharedPtr;

template<typename T, typename Allocator>
class WeakPtr;

template<typename T, typename Allocator>
class UnsafePtr;

template<typename T, typename Allocator>
struct SharedBlock {
	template<typename... Args>
	SharedBlock(Allocator &allocator, Args &&... args)
	: allocator(allocator), refCount(1), weakCount(1) {
		storage.construct(traits::forward<Args>(args)...);
	}

	SharedBlock(const SharedBlock &other) = delete;

	SharedBlock &operator= (const SharedBlock &other) = delete;

	~SharedBlock() {
		ASSERT(volatileRead<int>(&refCount) == 0);
		ASSERT(volatileRead<int>(&weakCount) == 0);
	}

	Allocator &allocator;
	int refCount;
	int weakCount;
	
	SharedStorage<T> storage;
};

template<typename T, typename Allocator>
class SharedPtr {
	friend class UnsafePtr<T, Allocator>;
public:
	template<typename... Args>
	static SharedPtr make(Allocator &allocator, Args &&... args) {
		auto block = memory::construct<SharedBlock<T, Allocator>>
				(allocator, allocator, traits::forward<Args>(args)...);
		return SharedPtr<T, Allocator>(block);
	}

	SharedPtr() : p_block(nullptr) { }
	
	~SharedPtr() {
		reset();
	}

	SharedPtr(const SharedPtr &other) {
		p_block = other.p_block;
		if(p_block != nullptr) {
			int old_ref_count;
			fetchInc(&p_block->refCount, old_ref_count);
			ASSERT(old_ref_count > 0);
		}
	}
	
	explicit SharedPtr(const WeakPtr<T, Allocator> &weak);
	
	explicit SharedPtr(const UnsafePtr<T, Allocator> &unsafe);

	SharedPtr(SharedPtr &&other) {
		p_block = other.p_block;
		other.p_block = nullptr;
	}

	SharedPtr &operator= (SharedPtr &&other) {
		p_block = other.p_block;
		other.p_block = nullptr;
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
			p_block->storage.destruct();

			int old_weak_count;
			fetchDec(&p_block->weakCount, old_weak_count);
			ASSERT(old_weak_count > 0);
			if(old_weak_count == 1)
				memory::destruct(p_block->allocator, p_block);
		}
		p_block = nullptr;
	}

	T *operator-> () const {
		if(p_block == nullptr)
			return nullptr;
		return &(*p_block->storage);
	}
	T *get() const {
		if(p_block == nullptr)
			return nullptr;
		return &(*p_block->storage);
	}

private:
	SharedPtr(SharedBlock<T, Allocator> *pointer) : p_block(pointer) { }

	SharedBlock<T, Allocator> *p_block;
};

template<typename T, typename Allocator>
class WeakPtr {
	friend class SharedPtr<T, Allocator>;
	friend class UnsafePtr<T, Allocator>;
public:
	WeakPtr() : p_block(nullptr) { }
	
	~WeakPtr() {
		reset();
	}

	WeakPtr(const WeakPtr &other) {
		p_block = other.p_block;
		if(p_block != nullptr) {
			int old_weak_count;
			fetchInc(&p_block->weakCount, old_weak_count);
			ASSERT(old_weak_count > 0);
		}
	}
	
	explicit WeakPtr(const UnsafePtr<T, Allocator> &unsafe);

	WeakPtr(WeakPtr &&other) {
		p_block = other.p_block;
		other.p_block = nullptr;
	}

	WeakPtr &operator= (WeakPtr &&other) {
		p_block = other.p_block;
		other.p_block = nullptr;
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
		ASSERT(old_weak_count > 0);
		if(old_weak_count == 1)
			memory::destruct(p_block->allocator, p_block);
		
		p_block = nullptr;
	}

private:
	SharedBlock<T, Allocator> *p_block;
};

template<typename T, typename Allocator>
class UnsafePtr {
	friend class WeakPtr<T, Allocator>;
	friend class SharedPtr<T, Allocator>;
public:
	UnsafePtr() : p_block(nullptr) { }
	
	UnsafePtr(const SharedPtr<T, Allocator> &shared)
	: p_block(shared.p_block) { }

	operator bool () {
		return p_block != nullptr;
	}

	T *operator-> () {
		if(p_block == nullptr)
			return nullptr;
		return &(*p_block->storage);
	}
	T *get() {
		if(p_block == nullptr)
			return nullptr;
		return &(*p_block->storage);
	}

private:
	SharedBlock<T, Allocator> *p_block;
};

template<typename T, typename Allocator>
SharedPtr<T, Allocator>::SharedPtr(const WeakPtr<T, Allocator> &weak)
: p_block(weak.p_block) {
	int last_ref_count = volatileRead<int>(&p_block->refCount);
	while(true) {
		if(last_ref_count == 0) {
			p_block = nullptr;
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
: p_block(unsafe.p_block) {
	int old_ref_count;
	fetchInc<int>(&p_block->refCount, old_ref_count);
	ASSERT(old_ref_count > 0);
}

template<typename T, typename Allocator>
WeakPtr<T, Allocator>::WeakPtr(const UnsafePtr<T, Allocator> &unsafe)
: p_block(unsafe.p_block) {
	int old_weak_count;
	fetchInc<int>(&p_block->weakCount, old_weak_count);
	ASSERT(old_weak_count > 0);
}

template<typename T, typename Allocator, typename... Args>
SharedPtr<T, Allocator> makeShared(Allocator &allocator, Args&&... args) {
	return SharedPtr<T, Allocator>::make(allocator, traits::forward<Args>(args)...);
}

} // namespace frigg

