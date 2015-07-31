
template<typename T, typename Allocator>
class SharedPtr;

template<typename T, typename Allocator>
class UnsafePtr;

template<typename T, typename Allocator>
class SharedBase {
	friend class SharedPtr<T, Allocator>;
	friend class UnsafePtr<T, Allocator>;

public:
	SharedBase() : p_allocator(nullptr),
			p_refCount(1) { }

	UnsafePtr<T, Allocator> thisPtr();

private:
	Allocator *p_allocator;
	int p_refCount;
};

template<typename T, typename Allocator>
class SharedPtr {
	friend class SharedBase<T, Allocator>;
	friend class UnsafePtr<T, Allocator>;
public:
	template<typename... Args>
	static SharedPtr make(Allocator &allocator, Args&&... args) {
		auto base = frigg::memory::construct<T>(allocator, frigg::traits::forward<Args>(args)...);
		base->p_allocator = &allocator;
		return SharedPtr<T, Allocator>(base);
	}

	SharedPtr() : p_pointer(nullptr) { }
	
	~SharedPtr() {
		if(p_pointer == nullptr)
			return;
		SharedBase<T, Allocator> *base = p_pointer;
		base->p_refCount--;
		if(base->p_refCount == 0)
			frigg::memory::destruct(*base->p_allocator, p_pointer);
	}

	SharedPtr(const SharedPtr &other) {
		p_pointer = other.p_pointer;
		if(p_pointer != nullptr)
			p_pointer->p_refCount++;
	}

	SharedPtr(SharedPtr &&other) {
		p_pointer = other.p_pointer;
		other.p_pointer = nullptr;
	}

	operator UnsafePtr<T, Allocator> ();

	SharedPtr &operator= (SharedPtr &&other) {
		p_pointer = other.p_pointer;
		other.p_pointer = nullptr;
		return *this;
	}

	T *operator-> () const {
		return p_pointer;
	}
	T *get() const {
		return p_pointer;
	}

private:
	SharedPtr(T *pointer) : p_pointer(pointer) { }

	T *p_pointer;
};

template<typename T, typename Allocator>
class UnsafePtr {
	friend class SharedBase<T, Allocator>;
	friend class SharedPtr<T, Allocator>;
public:
	UnsafePtr() : p_pointer(nullptr) { }
	
	operator SharedPtr<T, Allocator> ();

	T *operator-> () {
		return p_pointer;
	}
	T *get() {
		return p_pointer;
	}

private:
	UnsafePtr(T *pointer) : p_pointer(pointer) { }

	T *p_pointer;
};

template<typename T, typename Allocator>
UnsafePtr<T, Allocator> SharedBase<T, Allocator>::thisPtr() {
	return UnsafePtr<T, Allocator>(static_cast<T *>(this));
}

template<typename T, typename Allocator>
SharedPtr<T, Allocator>::operator UnsafePtr<T, Allocator>() {
	return UnsafePtr<T, Allocator>(p_pointer);
}

template<typename T, typename Allocator>
UnsafePtr<T, Allocator>::operator SharedPtr<T, Allocator>() {
	p_pointer->p_refCount++;
	return SharedPtr<T, Allocator>(p_pointer);
}

template<typename T, typename Allocator, typename... Args>
SharedPtr<T, Allocator> makeShared(Allocator &allocator, Args&&... args) {
	return SharedPtr<T, Allocator>::make(allocator, frigg::traits::forward<Args>(args)...);
}

