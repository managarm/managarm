
namespace helx {

inline void panic(const char *string) {
	int length = 0;
	for(int i = 0; string[i] != 0; i++)
		length++;
	helPanic(string, length);
}

typedef void (*RecvStringFunction) (void *, HelError, int64_t, int64_t, size_t);
typedef void (*RecvDescriptorFunction) (void *, HelError, int64_t, int64_t, HelHandle);
typedef void (*AcceptFunction) (void *, HelError, HelHandle);
typedef void (*ConnectFunction) (void *, HelError, HelHandle);
typedef void (*IrqFunction) (void *, HelError);

class EventHub {
public:
	enum { kEventsPerCall = 16 };

	inline EventHub() {
		HEL_CHECK(helCreateEventHub(&p_handle));
	}

	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void defaultProcessEvents(int64_t max_nanotime = kHelWaitInfinite) {
		HelEvent list[kEventsPerCall];
		size_t num_items;
		HEL_CHECK(helWaitForEvents(p_handle, list, kEventsPerCall,
				max_nanotime, &num_items));

		for(int i = 0; i < num_items; i++) {
			HelEvent &evt = list[i];
			switch(evt.type) {
			case kHelEventRecvString: {
				auto function = (RecvStringFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error,
						evt.msgRequest, evt.msgSequence, evt.length);
			} break;
			case kHelEventRecvDescriptor: {
				auto function = (RecvDescriptorFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error,
						evt.msgRequest, evt.msgSequence, evt.handle);
			} break;
			case kHelEventAccept: {
				auto function = (AcceptFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error, evt.handle);
			} break;
			case kHelEventConnect: {
				auto function = (ConnectFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error, evt.handle);
			} break;
			case kHelEventIrq: {
				auto function = (IrqFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error);
			} break;
			default:
				panic("Unknown event type");
			}
		}
	}

	inline HelEvent waitForEvent(int64_t async_id) {
		while(true) {
			HelEvent event;
			size_t num_items;
			HEL_CHECK(helWaitForEvents(p_handle, &event, 1,
					kHelWaitInfinite, &num_items));

			if(num_items == 0)
				continue;
			//ASSERT(event.asyncId == async_id);
			return event;
		}
	}

	inline size_t waitForRecvString(int64_t async_id) {
		HelEvent event = waitForEvent(async_id);
		return event.length;
	}
	inline HelHandle waitForRecvDescriptor(int64_t async_id) {
		HelEvent event = waitForEvent(async_id);
		return event.handle;
	}
	inline HelHandle waitForConnect(int64_t async_id) {
		HelEvent event = waitForEvent(async_id);
		return event.handle;
	}

private:
	HelHandle p_handle;
};

class Pipe {
public:
	static void createBiDirection(Pipe &first, Pipe &second) {
		HelHandle first_handle, second_handle;
		HEL_CHECK(helCreateBiDirectionPipe(&first_handle, &second_handle));
		first = Pipe(first_handle);
		second = Pipe(second_handle);
	}

	inline Pipe() : p_handle(kHelNullHandle) { }

	inline Pipe(HelHandle handle) : p_handle(handle) { }
	
	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void sendString(const void *buffer, size_t length,
			int64_t msg_request, int64_t msg_seq) {
		HEL_CHECK(helSendString(p_handle, (const uint8_t *)buffer, length,
				msg_request, msg_seq));
	}

	inline void sendDescriptor(HelHandle send_handle,
			int64_t msg_request, int64_t msg_seq) {
		HEL_CHECK(helSendDescriptor(p_handle, send_handle, msg_request, msg_seq));
	}

	inline void recvString(void *buffer, size_t max_length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			void *object, RecvStringFunction function) {
		int64_t async_id;
		HEL_CHECK(helSubmitRecvString(p_handle, event_hub.getHandle(),
				(uint8_t *)buffer, max_length, msg_request, msg_seq,
				(uintptr_t)function, (uintptr_t)object, &async_id));
	}

	inline void recvStringSync(void *buffer, size_t max_length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			size_t &length) {
		int64_t async_id;
		HEL_CHECK(helSubmitRecvString(p_handle, event_hub.getHandle(),
				(uint8_t *)buffer, max_length, msg_request, msg_seq,
				0, 0, &async_id));
		length = event_hub.waitForRecvString(async_id);
	}
	
	inline void recvDescriptor(EventHub &event_hub,
			int64_t msg_request, int64_t msg_seq,
			void *object, RecvDescriptorFunction function) {
		int64_t async_id;
		HEL_CHECK(helSubmitRecvDescriptor(p_handle, event_hub.getHandle(),
				msg_request, msg_seq, (uintptr_t)function, (uintptr_t)object, &async_id));
	}
	
	inline void recvDescriptorSync(EventHub &event_hub,
			int64_t msg_request, int64_t msg_seq, HelHandle &handle) {
		int64_t async_id;
		HEL_CHECK(helSubmitRecvDescriptor(p_handle, event_hub.getHandle(),
				msg_request, msg_seq, 0, 0, &async_id));
		handle = event_hub.waitForRecvDescriptor(async_id);
	}

private:
	HelHandle p_handle;
};

class Client {
public:
	inline Client() : p_handle(kHelNullHandle) { }

	inline Client(HelHandle handle) : p_handle(handle) { }

	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void connect(EventHub &event_hub,
			void *object, ConnectFunction function) {
		int64_t async_id;
		HEL_CHECK(helSubmitConnect(p_handle, event_hub.getHandle(),
				(uintptr_t)function, (uintptr_t)object, &async_id));
	}

private:
	HelHandle p_handle;
};

class Server {
public:
	static void createServer(Server &server, Client &client) {
		HelHandle server_handle, client_handle;
		HEL_CHECK(helCreateServer(&server_handle, &client_handle));
		server = Server(server_handle);
		client = Client(client_handle);
	}

	inline Server() : p_handle(kHelNullHandle) { }

	inline Server(HelHandle handle) : p_handle(handle) { }

	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void accept(EventHub &event_hub,
			void *object, AcceptFunction function) {
		int64_t async_id;
		HEL_CHECK(helSubmitAccept(p_handle, event_hub.getHandle(),
				(uintptr_t)function, (uintptr_t)object, &async_id));
	}

private:
	HelHandle p_handle;
};

class Directory {
public:
	static inline Directory create() {
		HelHandle handle;
		HEL_CHECK(helCreateRd(&handle));
		return Directory(handle);
	}

	inline Directory() : p_handle(kHelNullHandle) { }

	inline Directory(HelHandle handle) : p_handle(handle) { }

	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void mount(HelHandle mount_handle, const char *target) {
		HEL_CHECK(helRdMount(p_handle, target, strlen(target), mount_handle));
	}

	inline void publish(HelHandle publish_handle, const char *target) {
		HEL_CHECK(helRdPublish(p_handle, target, strlen(target), publish_handle));
	}

	void remount(const char *path, const char *target) {
		HelHandle mount_handle;
		HEL_CHECK(helRdOpen(path, strlen(path), &mount_handle));
		mount(mount_handle, target);
	}

private:
	HelHandle p_handle;
};

} // namespace helx

