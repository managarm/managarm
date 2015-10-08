
#ifndef FRIGG_CALLBACK_HPP
#define FRIGG_CALLBACK_HPP

#include <frigg/traits.hpp>
#include <frigg/memory.hpp>

namespace frigg {

template<typename Prototype>
class CallbackPtr;

template<typename R, typename... Args>
class CallbackPtr<R(Args...)> {
public:
	template<typename T, R (T::*pointer) (Args...)>
	static CallbackPtr ptrToMember(T *object) {
		struct Wrapper {
			static R invoke(void *object, Args... args) {
				return (static_cast<T *>(object)->*pointer)(args...);
			}
		};

		return CallbackPtr(object, &Wrapper::invoke);
	}

	template<typename T, R (*pointer) (T *, Args...)>
	static CallbackPtr staticPtr(T *object) {
		struct Wrapper {
			static R invoke(void *object, Args... args) {
				return pointer(static_cast<T *>(object), args...);
			}
		};

		return CallbackPtr(object, &Wrapper::invoke);
	}

	typedef R (*FunctionPtr) (void *, Args...);
	
	CallbackPtr()
	: object(nullptr), function(nullptr) { }

	CallbackPtr(void *object, FunctionPtr function)
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
	typedef CallbackPtr<R(Args...)> Type;

	template<R (T::*pointer) (Args...)>
	static CallbackPtr<R(Args...)> ptrToMember(T *object) {
		return CallbackPtr<R(Args...)>::template ptrToMember<T, pointer>(object);
	}
};

template<typename StaticType>
struct CallbackStatic;

template<typename R, typename T, typename... Args>
struct CallbackStatic<R (*) (T *, Args...)> {
	typedef CallbackPtr<R(Args...)> Type;

	template<R (*pointer) (T *, Args...)>
	static CallbackPtr<R(Args...)> staticPtr(T *object) {
		return CallbackPtr<R(Args...)>::template staticPtr<T, pointer>(object);
	}
};

#define CALLBACK_MEMBER(object, pointer) ::frigg::CallbackMember<decltype(pointer)>::template ptrToMember<pointer>(object)
#define CALLBACK_STATIC(object, pointer) ::frigg::CallbackStatic<decltype(pointer)>::template staticPtr<pointer>(object)

template<typename R, typename ArgPack>
struct CallbackFromPack;

template<typename R, typename... Args>
struct CallbackFromPack<R, TypePack<Args...>> {
	typedef CallbackPtr<R(Args...)> Type;
};

template<typename C>
struct BaseClosure {
protected:
	template<typename Allocator>
	void suicide(Allocator &allocator) {
		destruct(allocator, static_cast<C *>(this));
	}
};

template<typename C, typename Allocator, typename... Args>
static void runClosure(Allocator &allocator, Args &&... args) {
	auto closure = construct<C>(allocator, forward<Args>(args)...);
	(*closure)();
}

} // namespace frigg

#endif // FRIGG_CALLBACK_HPP

