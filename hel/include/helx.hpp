
namespace helx {

inline void panic(const char *string) {
	int length = 0;
	for(int i = 0; string[i] != 0; i++)
		length++;
	helPanic(string, length);
}

template<typename... Args>
class Callback {
public:
	typedef void (*FunctionPtr) (void *, Args...);

	Callback() : p_object(nullptr), p_function(nullptr) { }

	Callback(void *object, FunctionPtr function)
			: p_object(object), p_function(function) { }
	
	template<typename T, void (T::*function) (Args...)>
	static Callback make(T *object) {
		struct Wrapper {
			static void run(void *object, Args... args) {
				auto ptr = static_cast<T *>(object);
				(ptr->*function)(args...);
			}
		};

		return Callback(object, &Wrapper::run);
	}
	
	template<void (*function) (Args...)>
	static Callback make() {
		struct Wrapper {
			static void run(void *object, Args... args) {
				function(args...);
			}
		};

		return Callback(nullptr, &Wrapper::run);
	}

	inline void operator() (Args... args) {
		p_function(p_object, args...);
	}
	
	inline void *getObject() {
		return p_object;
	}
	inline FunctionPtr getFunction() {
		return p_function;
	}

private:
	void *p_object;
	FunctionPtr p_function;
};

template<typename Prototype, Prototype f>
struct MemberHelper;

template<typename Object, typename... Args, void (Object::*function) (Args...)>
struct MemberHelper<void (Object::*) (Args...), function> {
	static Callback<Args...> make(Object *object) {
			return Callback<Args...>::template make<Object, function>(object);
	}
};

#define HELX_MEMBER(x, f) ::helx::MemberHelper<decltype(f), f>::make(x)

typedef Callback<int64_t, HelError, size_t> RecvStringCb;
typedef Callback<int64_t, HelHandle> RecvDescriptorCb;
typedef Callback<int64_t, HelHandle> AcceptCb;
typedef Callback<int64_t, HelHandle> ConnectCb;
typedef Callback<int64_t> IrqCb;

class EventHub {
public:
	enum { kEventsPerCall = 16 };

	inline EventHub() {
		int error = helCreateEventHub(&p_handle);
		if(error != kHelErrNone)
			panic("helCreateEventHub() failed");
	}

	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void defaultProcessEvents() {
		HelEvent list[kEventsPerCall];
		size_t num_items;
		int error = helWaitForEvents(p_handle, list, kEventsPerCall,
				kHelWaitInfinite, &num_items);
		if(error != kHelErrNone)
			panic("helWaitForEvents() failed");

		for(int i = 0; i < num_items; i++) {
			HelEvent &evt = list[i];
			switch(evt.type) {
			case kHelEventRecvString: {
				auto cb = (RecvStringCb::FunctionPtr)evt.submitFunction;
				cb((void *)evt.submitObject, evt.submitId, evt.error, evt.length);
			} break;
			case kHelEventAccept: {
				auto cb = (AcceptCb::FunctionPtr)evt.submitFunction;
				cb((void *)evt.submitObject, evt.submitId, evt.handle);
			} break;
			case kHelEventConnect: {
				auto cb = (ConnectCb::FunctionPtr)evt.submitFunction;
				cb((void *)evt.submitObject, evt.submitId, evt.handle);
			} break;
			case kHelEventIrq: {
				auto cb = (IrqCb::FunctionPtr)evt.submitFunction;
				cb((void *)evt.submitObject, evt.submitId);
			} break;
			default:
				panic("Unknown event type");
			}
		}
	}

	inline HelEvent waitForEvent(int64_t submit_id) {
		while(true) {
			HelEvent event;
			size_t num_items;
			int error = helWaitForEvents(p_handle, &event, 1,
					kHelWaitInfinite, &num_items);
			if(error != kHelErrNone)
				panic("helWaitForEvents() failed");

			if(num_items == 0)
				continue;
			//FIXME: ASSERT(event.submitId == submit_id);
			return event;
		}
	}

	inline size_t waitForRecvString(int64_t submit_id) {
		HelEvent event = waitForEvent(submit_id);
		return event.length;
	}
	inline HelHandle waitForRecvDescriptor(int64_t submit_id) {
		HelEvent event = waitForEvent(submit_id);
		return event.handle;
	}
	inline HelHandle waitForConnect(int64_t submit_id) {
		HelEvent event = waitForEvent(submit_id);
		return event.handle;
	}

private:
	HelHandle p_handle;
};

class Pipe {
public:
	inline Pipe(HelHandle handle) : p_handle(handle) { }
	
	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void sendString(const void *buffer, size_t length,
			int64_t msg_request, int64_t msg_seq) {
		helSendString(p_handle, (const uint8_t *)buffer, length,
				msg_request, msg_seq);
	}

	inline void sendDescriptor(HelHandle send_handle,
			int64_t msg_request, int64_t msg_seq) {
		helSendDescriptor(p_handle, send_handle, msg_request, msg_seq);
	}

	inline void recvString(void *buffer, size_t length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			RecvStringCb callback) {
		helSubmitRecvString(p_handle, event_hub.getHandle(),
				(uint8_t *)buffer, length, msg_request, length, 0,
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject());
	}

private:
	HelHandle p_handle;
};

class Server {
public:
	inline Server(HelHandle handle) : p_handle(handle) { }

	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void accept(EventHub &event_hub, AcceptCb callback) {
		helSubmitAccept(p_handle, event_hub.getHandle(), 0,
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject());
	}

private:
	HelHandle p_handle;
};

} // namespace helx

