
namespace frigg {
namespace util {

template<typename Prototype>
class FuncPtr;

template<typename R, typename... Args>
class FuncPtr<R(Args...)> {
public:
	template<typename T, R (T::*pointer) (Args...)>
	static FuncPtr ptrToMember(T *object) {
		struct Wrapper {
			static R invoke(void *object, Args... args) {
				return (static_cast<T *>(object)->*pointer)(args...);
			}
		};

		return FuncPtr(object, &Wrapper::invoke);
	}

	template<typename T, R (*pointer) (T *, Args...)>
	static FuncPtr staticPtr(T *object) {
		struct Wrapper {
			static R invoke(void *object, Args... args) {
				return pointer(static_cast<T *>(object), args...);
			}
		};

		return FuncPtr(object, &Wrapper::invoke);
	}

	typedef R (*FunctionPtr) (void *, Args...);

	FuncPtr(void *object, FunctionPtr function)
	: object(object), function(function) { }
	
	void *getObject() {
		return object;
	}
	FunctionPtr getFunction() {
		return function;
	}

	R operator() (Args... args) {
		return function(object, args...);
	}

private:
	void *object;
	FunctionPtr function;
};

template<typename MemberType>
struct MemberToFuncPtr;

template<typename T, typename R, typename... Args>
struct MemberToFuncPtr<R (T::*) (Args...)> {
	typedef FuncPtr<R(Args...)> Type;

	template<R (T::*pointer) (Args...)>
	static FuncPtr<R(Args...)> ptrToMember(T *object) {
		return FuncPtr<R(Args...)>::template ptrToMember<T, pointer>(object);
	}
};

template<typename StaticType>
struct StaticToFuncPtr;

template<typename R, typename T, typename... Args>
struct StaticToFuncPtr<R (*) (T *, Args...)> {
	typedef FuncPtr<R(Args...)> Type;

	template<R (*pointer) (T *, Args...)>
	static FuncPtr<R(Args...)> staticPtr(T *object) {
		return FuncPtr<R(Args...)>::template staticPtr<T, pointer>(object);
	}
};

#define FUNCPTR_MEMBER(object, pointer) ::frigg::util::MemberToFuncPtr<decltype(pointer)>::template ptrToMember<pointer>(object)
#define FUNCPTR_STATIC(object, pointer) ::frigg::util::StaticToFuncPtr<decltype(pointer)>::template staticPtr<pointer>(object)

template<typename R, typename ArgPack>
struct FuncPtrFromPack;

template<typename R, typename... Args>
struct FuncPtrFromPack<R, traits::TypePack<Args...>> {
	typedef FuncPtr<R(Args...)> Type;
};

} } // namespace frigg::util

