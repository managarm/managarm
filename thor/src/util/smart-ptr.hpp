
template<typename T>
class SharedPtr;

template<typename T>
class UnsafePtr;

class SharedObject {
public:
	template<typename T = SharedObject>
	SharedPtr<T> shared();

	template<typename T = SharedObject>
	UnsafePtr<T> unsafe();
};

template<typename T>
class SharedPtr {
	friend class SharedObject;
public:
	SharedPtr() : p_pointer(nullptr) { }

	SharedPtr(const SharedPtr &other) = delete;

	SharedPtr(SharedPtr &&other) {
		p_pointer = other.p_pointer;
		other.p_pointer = nullptr;
	}

	SharedPtr &operator= (const SharedPtr &other) = delete;

	SharedPtr &operator= (SharedPtr &&other) {
		p_pointer = other.p_pointer;
		other.p_pointer = nullptr;
		return *this;
	}

	T *operator-> () {
		return p_pointer;
	}

private:
	SharedPtr(T *pointer) : p_pointer(pointer) { }

	T *p_pointer;
};

template<typename T>
class UnsafePtr {
	friend class SharedObject;
public:
	UnsafePtr() : p_pointer(nullptr) { }

	T *operator-> () {
		return p_pointer;
	}

private:
	UnsafePtr(T *pointer) : p_pointer(pointer) { }

	T *p_pointer;
};

template<typename T>
SharedPtr<T> SharedObject::shared() {
	return SharedPtr<T>(static_cast<T *>(this));
}

template<typename T>
UnsafePtr<T> SharedObject::unsafe() {
	return UnsafePtr<T>(static_cast<T *>(this));
}

template<typename T, typename Allocator, typename... Args>
SharedPtr<T> makeShared(Allocator *allocator, Args&&... args) {
	auto pointer = new (allocator) T(thor::util::forward<Args>(args)...);
	return pointer->template shared<T>();
}

