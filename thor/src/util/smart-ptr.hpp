
template<typename T>
class SharedPtr;

template<typename T>
class UnsafePtr;

template<typename T>
class SharedBase {
public:
	UnsafePtr<T> thisPtr();
};

template<typename T>
class SharedPtr {
	friend class SharedBase<T>;
	friend class UnsafePtr<T>;
public:
	template<typename Allocator, typename... Args>
	static SharedPtr<T> make(Allocator &allocator, Args&&... args) {
		auto base = construct<T>(allocator, thor::util::forward<Args>(args)...);
		return SharedPtr<T>(base);
	}

	SharedPtr() : p_pointer(nullptr) { }
	
	SharedPtr(const SharedPtr &other) {
		p_pointer = other.p_pointer;
		//FIXME: update refcount
	}

	SharedPtr(SharedPtr &&other) {
		p_pointer = other.p_pointer;
		other.p_pointer = nullptr;
	}

	operator UnsafePtr<T> ();

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

template<typename T>
class UnsafePtr {
	friend class SharedBase<T>;
	friend class SharedPtr<T>;
public:
	UnsafePtr() : p_pointer(nullptr) { }
	
	operator SharedPtr<T> ();

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

template<typename T>
UnsafePtr<T> SharedBase<T>::thisPtr() {
	//FIXME
	return UnsafePtr<T>(static_cast<T *>(this));
}

template<typename T>
SharedPtr<T>::operator UnsafePtr<T>() {
	return UnsafePtr<T>(p_pointer);
}

template<typename T>
UnsafePtr<T>::operator SharedPtr<T>() {
	//FIXME: increment refcount
	return SharedPtr<T>(p_pointer);
}

template<typename T, typename Allocator, typename... Args>
SharedPtr<T> makeShared(Allocator &allocator, Args&&... args) {
	return SharedPtr<T>::make(allocator, thor::util::forward<Args>(args)...);
}

