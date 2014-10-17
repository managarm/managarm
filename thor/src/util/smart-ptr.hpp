
template<typename T>
class RefCountPtr {
private:
	struct Wrapper {
		template<typename... Args>
		Wrapper(Args... args)
				: object(args...) { }

		T object;
	};

public:
	RefCountPtr()
			: p_pointer(nullptr) { }
	
	template<typename Allocator, typename... Args>
	static RefCountPtr<T> make(Allocator *allocator, Args... args) {
		return RefCountPtr<T>(new (allocator) Wrapper(args...));
	}

	T *operator-> () {
		return &p_pointer->object;
	}

private:
	RefCountPtr(Wrapper *pointer)
			: p_pointer(pointer) { }

	Wrapper *p_pointer;
};

