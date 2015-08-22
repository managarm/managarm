
template<typename T, typename Allocator>
class SharedPtr;

template<typename T, typename Allocator>
class UnsafePtr;

template<typename T, typename Allocator>
struct SharedBlock {
	template<typename... Args>
	SharedBlock(Allocator &allocator, Args &&... args)
	: allocator(allocator), refCount(1),
			object(frigg::traits::forward<Args>(args)...) { }

	SharedBlock(const SharedBlock &other) = delete;

	SharedBlock &operator= (const SharedBlock &other) = delete;

	Allocator &allocator;
	int refCount;
	
	T object;
};

template<typename T, typename Allocator>
class SharedPtr {
	friend class UnsafePtr<T, Allocator>;
public:
	template<typename... Args>
	static SharedPtr make(Allocator &allocator, Args &&... args) {
		auto block = frigg::memory::construct<SharedBlock<T, Allocator>>
				(allocator, allocator, frigg::traits::forward<Args>(args)...);
		return SharedPtr<T, Allocator>(block);
	}

	SharedPtr() : p_block(nullptr) { }
	
	~SharedPtr() {
		reset();
	}

	SharedPtr(const SharedPtr &other) {
		p_block = other.p_block;
		if(p_block != nullptr)
			p_block->refCount++;
	}

	SharedPtr(SharedPtr &&other) {
		p_block = other.p_block;
		other.p_block = nullptr;
	}

	operator UnsafePtr<T, Allocator> ();

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
		p_block->refCount--;
		if(p_block->refCount == 0)
			frigg::memory::destruct(p_block->allocator, p_block);
		p_block = nullptr;
	}

	T *operator-> () const {
		if(p_block == nullptr)
			return nullptr;
		return &p_block->object;
	}
	T *get() const {
		if(p_block == nullptr)
			return nullptr;
		return &p_block->object;
	}

private:
	SharedPtr(SharedBlock<T, Allocator> *pointer) : p_block(pointer) { }

	SharedBlock<T, Allocator> *p_block;
};

template<typename T, typename Allocator>
class UnsafePtr {
	friend class SharedPtr<T, Allocator>;
public:
	UnsafePtr() : p_block(nullptr) { }
	
	operator SharedPtr<T, Allocator> ();

	operator bool () {
		return p_block != nullptr;
	}

	T *operator-> () {
		if(p_block == nullptr)
			return nullptr;
		return &p_block->object;
	}
	T *get() {
		if(p_block == nullptr)
			return nullptr;
		return &p_block->object;
	}

private:
	UnsafePtr(SharedBlock<T, Allocator> *pointer) : p_block(pointer) { }

	SharedBlock<T, Allocator> *p_block;
};

template<typename T, typename Allocator>
SharedPtr<T, Allocator>::operator UnsafePtr<T, Allocator>() {
	return UnsafePtr<T, Allocator>(p_block);
}

template<typename T, typename Allocator>
UnsafePtr<T, Allocator>::operator SharedPtr<T, Allocator>() {
	p_block->refCount++;
	return SharedPtr<T, Allocator>(p_block);
}

template<typename T, typename Allocator, typename... Args>
SharedPtr<T, Allocator> makeShared(Allocator &allocator, Args&&... args) {
	return SharedPtr<T, Allocator>::make(allocator, frigg::traits::forward<Args>(args)...);
}

