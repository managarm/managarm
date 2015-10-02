
namespace frigg {
namespace util {

template<typename Prototype>
class Callback;

template<typename R, typename... Args>
class Callback<R(Args...)> {
public:
	template<typename T, R (T::*pointer) (Args...)>
	static Callback ptrToMember(T *object) {
		struct Wrapper {
			static R invoke(void *object, Args... args) {
				return (static_cast<T *>(object)->*pointer)(args...);
			}
		};

		return Callback(object, &Wrapper::invoke);
	}

	template<typename T, R (*pointer) (T *, Args...)>
	static Callback staticPtr(T *object) {
		struct Wrapper {
			static R invoke(void *object, Args... args) {
				return pointer(static_cast<T *>(object), args...);
			}
		};

		return Callback(object, &Wrapper::invoke);
	}

	typedef R (*FunctionPtr) (void *, Args...);
	
	Callback()
	: object(nullptr), function(nullptr) { }

	Callback(void *object, FunctionPtr function)
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
struct CallbackMember;

template<typename T, typename R, typename... Args>
struct CallbackMember<R (T::*) (Args...)> {
	typedef Callback<R(Args...)> Type;

	template<R (T::*pointer) (Args...)>
	static Callback<R(Args...)> ptrToMember(T *object) {
		return Callback<R(Args...)>::template ptrToMember<T, pointer>(object);
	}
};

template<typename StaticType>
struct CallbackStatic;

template<typename R, typename T, typename... Args>
struct CallbackStatic<R (*) (T *, Args...)> {
	typedef Callback<R(Args...)> Type;

	template<R (*pointer) (T *, Args...)>
	static Callback<R(Args...)> staticPtr(T *object) {
		return Callback<R(Args...)>::template staticPtr<T, pointer>(object);
	}
};

#define CALLBACK_MEMBER(object, pointer) ::frigg::util::CallbackMember<decltype(pointer)>::template ptrToMember<pointer>(object)
#define CALLBACK_STATIC(object, pointer) ::frigg::util::CallbackStatic<decltype(pointer)>::template staticPtr<pointer>(object)

template<typename R, typename ArgPack>
struct CallbackFromPack;

template<typename R, typename... Args>
struct CallbackFromPack<R, TypePack<Args...>> {
	typedef Callback<R(Args...)> Type;
};

} } // namespace frigg::util

