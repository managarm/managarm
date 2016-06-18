
#ifndef HELX_HPP
#define HELX_HPP

#include <frigg/cxx-support.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/callback.hpp>
#include <frigg/chain-all.hpp>

#include <hel.h>
#include <hel-syscalls.h>

namespace helx {

inline void panic(const char *string) {
	int length = 0;
	for(int i = 0; string[i] != 0; i++)
		length++;
	helPanic(string, length);
}

typedef void (*LoadMemoryFunction) (void *, HelError, uintptr_t, size_t);
typedef void (*LockMemoryFunction) (void *, HelError);
typedef void (*JoinFunction) (void *, HelError);
typedef void (*SendStringFunction) (void *, HelError);
typedef void (*SendDescriptorFunction) (void *, HelError);
typedef void (*RecvStringFunction) (void *, HelError, int64_t, int64_t, size_t);
typedef void (*RecvStringToQueueFunction) (void *, HelError, int64_t, int64_t, size_t, size_t, size_t);
typedef void (*RecvDescriptorFunction) (void *, HelError, int64_t, int64_t, HelHandle);
typedef void (*AcceptFunction) (void *, HelError, HelHandle);
typedef void (*ConnectFunction) (void *, HelError, HelHandle);
typedef void (*IrqFunction) (void *, HelError);

class Pipe;
class Client;
class Server;

class EventHub {
public:
	static inline EventHub create() {
		HelHandle handle;
		HEL_CHECK(helCreateEventHub(&handle));
		return EventHub(handle);
	}

	enum { kEventsPerCall = 16 };

	inline EventHub()
	: p_handle(kHelNullHandle) { }

	explicit inline EventHub(HelHandle handle)
	: p_handle(handle) { }
	
	inline EventHub(EventHub &&other)
	: EventHub() {
		swap(*this, other);
	}

	inline EventHub(const EventHub &other) = delete;

	inline ~EventHub() {
		reset();
	}
	
	inline EventHub &operator= (EventHub other) {
		swap(*this, other);
		return *this;
	}

	void reset() {
		if(p_handle != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(p_handle));
		p_handle = kHelNullHandle;
	}

	friend inline void swap(EventHub &a, EventHub &b) {
		using frigg::swap;
		swap(a.p_handle, b.p_handle);
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
			case kHelEventLoadMemory: {
				auto function = (LoadMemoryFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error, evt.offset, evt.length);
			} break;
			case kHelEventLockMemory: {
				auto function = (LockMemoryFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error);
			} break;
			case kHelEventJoin: {
				auto function = (JoinFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error);
			} break;
			case kHelEventSendString: {
				auto function = (SendStringFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error);
			} break;
			case kHelEventSendDescriptor: {
				auto function = (SendDescriptorFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error);
			} break;
			case kHelEventRecvString: {
				auto function = (RecvStringFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error,
						evt.msgRequest, evt.msgSequence, evt.length);
			} break;
			case kHelEventRecvStringToQueue: {
				// FIXME: fill in correct queue index
				auto function = (RecvStringToQueueFunction)evt.submitFunction;
				function((void *)evt.submitObject, evt.error,
						evt.msgRequest, evt.msgSequence, 0, evt.offset, evt.length);
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
			assert(event.asyncId == async_id);
			return event;
		}
	}

	inline void waitForSendString(int64_t async_id, HelError &error);
	inline void waitForSendDescriptor(int64_t async_id, HelError &error);
	inline void waitForRecvString(int64_t async_id, HelError &error, size_t &length);
	inline void waitForRecvDescriptor(int64_t async_id, HelError &error, HelHandle &handle);
	inline void waitForConnect(int64_t async_id, HelError &error, Pipe &pipe);

private:
	HelHandle p_handle;
};

class Pipe {
public:
	static inline void createFullPipe(Pipe &first, Pipe &second) {
		HelHandle first_handle, second_handle;
		HEL_CHECK(helCreateFullPipe(&first_handle, &second_handle));
		first = Pipe(first_handle);
		second = Pipe(second_handle);
	}

	inline Pipe() : p_handle(kHelNullHandle) { }

	inline Pipe(Pipe &&other)
	: Pipe() {
		swap(*this, other);
	}

	inline Pipe(const Pipe &other) = delete;

	explicit inline Pipe(HelHandle handle) : p_handle(handle) { }

	inline ~Pipe() {
		reset();
	}

	inline Pipe &operator= (Pipe other) {
		swap(*this, other);
		return *this;
	}

	inline void reset() {
		if(p_handle != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(p_handle));
		p_handle = kHelNullHandle;
	}

	friend inline void swap(Pipe &a, Pipe &b) {
		using frigg::swap;
		swap(a.p_handle, b.p_handle);
	}

	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void sendString(const void *buffer, size_t length,
			int64_t msg_request, int64_t msg_seq, uint32_t flags) {
		assert(!"Replace by async overloads");
	}
	inline void sendStringReq(const void *buffer, size_t length,
			int64_t msg_request, int64_t msg_seq) {
		assert(!"Replace by async overloads");
	}
	inline void sendStringResp(const void *buffer, size_t length,
			int64_t msg_request, int64_t msg_seq) {
		assert(!"Replace by async overloads");
	}

	inline void sendString(const void *buffer, size_t length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError)> callback,
			uint32_t flags) {
		int64_t async_id;
		HEL_CHECK(helSubmitSendString(p_handle, event_hub.getHandle(),
				(uint8_t *)buffer, length, msg_request, msg_seq,
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
				flags, &async_id));
	}
	inline void sendStringReq(const void *buffer, size_t length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError)> callback) {
		sendString(buffer, length, event_hub, msg_request, msg_seq,
				callback, kHelRequest);
	}
	inline void sendStringResp(const void *buffer, size_t length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError)> callback) {
		sendString(buffer, length, event_hub, msg_request, msg_seq,
				callback, kHelResponse);
	}
	
	inline auto sendStringResp(const void *buffer, size_t length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq) {
		// FIXME: do not capture event_hub by reference
		return frigg::await<void(HelError)>([=, &event_hub] (auto callback) {
			this->sendStringResp(buffer, length, event_hub, msg_request, msg_seq, callback);
		});
	}

	inline void sendStringSync(const void *buffer, size_t length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			uint32_t flags, HelError &error) {
		int64_t async_id;
		HEL_CHECK(helSubmitSendString(p_handle, event_hub.getHandle(),
				(uint8_t *)buffer, length, msg_request, msg_seq,
				0, 0, flags, &async_id));
		event_hub.waitForSendString(async_id, error);
	}
	inline void sendStringReqSync(const void *buffer, size_t length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			HelError &error) {
		sendStringSync(buffer, length, event_hub, msg_request, msg_seq,
				kHelRequest, error);
	}
	inline void sendStringRespSync(const void *buffer, size_t length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			HelError &error) {
		sendStringSync(buffer, length, event_hub, msg_request, msg_seq,
				kHelResponse, error);
	}

	inline void sendDescriptor(HelHandle send_handle,
			int64_t msg_request, int64_t msg_seq, uint32_t flags) {
		assert(!"Replace by async overloads");
	}
	inline void sendDescriptorReq(HelHandle send_handle,
			int64_t msg_request, int64_t msg_seq) {
		assert(!"Replace by async overloads");
	}
	inline void sendDescriptorResp(HelHandle send_handle,
			int64_t msg_request, int64_t msg_seq) {
		assert(!"Replace by async overloads");
	}
	
	inline void sendDescriptor(HelHandle send_handle,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq, uint32_t flags,
			frigg::CallbackPtr<void(HelError)> callback) {
		int64_t async_id;
		HEL_CHECK(helSubmitSendDescriptor(p_handle, event_hub.getHandle(),
				send_handle, msg_request, msg_seq,
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
				flags, &async_id));
	}
	inline void sendDescriptorReq(HelHandle send_handle,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError)> callback) {
		sendDescriptor(send_handle, event_hub, msg_request, msg_seq,
				kHelRequest, callback);
	}
	inline void sendDescriptorResp(HelHandle send_handle,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError)> callback) {
		sendDescriptor(send_handle, event_hub, msg_request, msg_seq,
				kHelResponse, callback);
	}

	inline void sendDescriptorSync(HelHandle send_handle,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			uint32_t flags, HelError &error) {
		int64_t async_id;
		HEL_CHECK(helSubmitSendDescriptor(p_handle, event_hub.getHandle(),
				send_handle, msg_request, msg_seq,
				0, 0, flags, &async_id));
		event_hub.waitForSendString(async_id, error);
	}
	inline void sendDescriptorReqSync(HelHandle send_handle,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq, HelError &error) {
		sendDescriptorSync(send_handle, event_hub, msg_request, msg_seq, kHelRequest, error);
	}
	inline void sendDescriptorRespSync(HelHandle send_handle,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq, HelError &error) {
		sendDescriptorSync(send_handle, event_hub, msg_request, msg_seq, kHelResponse, error);
	}

	inline HelError recvString(void *buffer, size_t max_length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError, int64_t, int64_t, size_t)> callback,
			uint32_t flags) {
		int64_t async_id;
		return helSubmitRecvString(p_handle, event_hub.getHandle(),
				(uint8_t *)buffer, max_length, msg_request, msg_seq,
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
				flags, &async_id);
	}
	inline HelError recvStringToRing(HelHandle ring_handle,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError, int64_t, int64_t, size_t, size_t, size_t)> callback,
			uint32_t flags) {
		int64_t async_id;
		return helSubmitRecvStringToRing(p_handle, event_hub.getHandle(),
				ring_handle, msg_request, msg_seq,
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
				flags, &async_id);
	}
	inline HelError recvStringReq(void *buffer, size_t max_length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError, int64_t, int64_t, size_t)> callback) {
		return recvString(buffer, max_length, event_hub,
				msg_request, msg_seq, callback, kHelRequest);
	}
	inline HelError recvStringReqToRing(HelHandle ring_handle,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError, int64_t, int64_t, size_t, size_t, size_t)> callback) {
		return recvStringToRing(ring_handle, event_hub,
				msg_request, msg_seq, callback, kHelRequest);
	}
	inline HelError recvStringResp(void *buffer, size_t max_length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError, int64_t, int64_t, size_t)> callback) {
		return recvString(buffer, max_length, event_hub,
				msg_request, msg_seq, callback, kHelResponse);
	}

	inline void recvStringSync(void *buffer, size_t max_length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			uint32_t flags, HelError &error, size_t &length) {
		int64_t async_id;
		HelError submit_error = helSubmitRecvString(p_handle, event_hub.getHandle(),
				(uint8_t *)buffer, max_length, msg_request, msg_seq,
				0, 0, flags, &async_id);
		if(submit_error != kHelErrNone) {
			error = submit_error;
			return;
		}
		event_hub.waitForRecvString(async_id, error, length);
	}
	inline void recvStringReqSync(void *buffer, size_t max_length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			HelError &error, size_t &length) {
		recvStringSync(buffer, max_length, event_hub, msg_request, msg_seq, kHelRequest,
				error, length);
	}
	inline void recvStringRespSync(void *buffer, size_t max_length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			HelError &error, size_t &length) {
		recvStringSync(buffer, max_length, event_hub, msg_request, msg_seq, kHelResponse,
				error, length);
	}
	
	inline void recvDescriptor(EventHub &event_hub,
			int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError, int64_t, int64_t, HelHandle)> callback,
			uint32_t flags) {
		int64_t async_id;
		HEL_CHECK(helSubmitRecvDescriptor(p_handle, event_hub.getHandle(),
				msg_request, msg_seq,
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
				flags, &async_id));
	}
	inline void recvDescriptorReq(EventHub &event_hub,
			int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError, int64_t, int64_t, HelHandle)> callback) {
		recvDescriptor(event_hub, msg_request, msg_seq, callback, kHelRequest);
	}
	inline void recvDescriptorResp(EventHub &event_hub,
			int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError, int64_t, int64_t, HelHandle)> callback) {
		recvDescriptor(event_hub, msg_request, msg_seq, callback, kHelResponse);
	}
	
	inline void recvDescriptorSync(EventHub &event_hub,
			int64_t msg_request, int64_t msg_seq,
			uint32_t flags, HelError &error, HelHandle &handle) {
		int64_t async_id;
		HEL_CHECK(helSubmitRecvDescriptor(p_handle, event_hub.getHandle(),
				msg_request, msg_seq, 0, 0, flags, &async_id));
		event_hub.waitForRecvDescriptor(async_id, error, handle);
	}
	inline void recvDescriptorReqSync(EventHub &event_hub,
			int64_t msg_request, int64_t msg_seq,
			HelError &error, HelHandle &handle) {
		recvDescriptorSync(event_hub, msg_request, msg_seq, kHelRequest,
				error, handle);
	}
	inline void recvDescriptorRespSync(EventHub &event_hub,
			int64_t msg_request, int64_t msg_seq,
			HelError &error, HelHandle &handle) {
		recvDescriptorSync(event_hub, msg_request, msg_seq, kHelResponse,
				error, handle);
	}

private:
	HelHandle p_handle;
};

class Client {
public:
	inline Client() : p_handle(kHelNullHandle) { }

	explicit inline Client(HelHandle handle) : p_handle(handle) { }
	
	inline Client(Client &&other)
	: Client() {
		swap(*this, other);
	}

	inline Client(const Client &other) = delete;

	inline ~Client() {
		reset();
	}
	
	inline Client &operator= (Client other) {
		swap(*this, other);
		return *this;
	}

	void reset() {
		if(p_handle != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(p_handle));
		p_handle = kHelNullHandle;
	}

	friend inline void swap(Client &a, Client &b) {
		using frigg::swap;
		swap(a.p_handle, b.p_handle);
	}

	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void connect(EventHub &event_hub,
			frigg::CallbackPtr<void(HelError, HelHandle)> callback) {
		int64_t async_id;
		HEL_CHECK(helSubmitConnect(p_handle, event_hub.getHandle(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(), &async_id));
	}
	
	inline void connectSync(EventHub &event_hub, HelError &error, Pipe &pipe) {
		int64_t async_id;
		HEL_CHECK(helSubmitConnect(p_handle, event_hub.getHandle(),
				0, 0, &async_id));
		event_hub.waitForConnect(async_id, error, pipe);
	}

private:
	HelHandle p_handle;
};

class Server {
public:
	static inline void createServer(Server &server, Client &client) {
		HelHandle server_handle, client_handle;
		HEL_CHECK(helCreateServer(&server_handle, &client_handle));
		server = Server(server_handle);
		client = Client(client_handle);
	}

	inline Server() : p_handle(kHelNullHandle) { }

	explicit inline Server(HelHandle handle) : p_handle(handle) { }
	
	inline Server(Server &&other)
	: Server() {
		swap(*this, other);
	}

	inline Server(const Server &other) = delete;

	inline ~Server() {
		reset();
	}
	
	inline Server &operator= (Server other) {
		swap(*this, other);
		return *this;
	}

	void reset() {
		if(p_handle != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(p_handle));
		p_handle = kHelNullHandle;
	}

	friend inline void swap(Server &a, Server &b) {
		using frigg::swap;
		swap(a.p_handle, b.p_handle);
	}

	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void accept(EventHub &event_hub,
			frigg::CallbackPtr<void(HelError, HelHandle)> callback) {
		int64_t async_id;
		HEL_CHECK(helSubmitAccept(p_handle, event_hub.getHandle(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(), &async_id));
	}

private:
	HelHandle p_handle;
};

class Irq {
public:
	inline static Irq access(int number) {
		HelHandle handle;
		HEL_CHECK(helAccessIrq(number, &handle));
		return Irq(handle);
	}

	inline Irq() : p_handle(kHelNullHandle) { }

	explicit inline Irq(HelHandle handle) : p_handle(handle) { }
	
	inline Irq(Irq &&other)
	: Irq() {
		swap(*this, other);
	}

	inline Irq(const Irq &other) = delete;

	inline ~Irq() {
		reset();
	}
	
	inline Irq &operator= (Irq other) {
		swap(*this, other);
		return *this;
	}

	void reset() {
		if(p_handle != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(p_handle));
		p_handle = kHelNullHandle;
	}

	friend inline void swap(Irq &a, Irq &b) {
		using frigg::swap;
		swap(a.p_handle, b.p_handle);
	}

	inline HelHandle getHandle() {
		return p_handle;
	}

	inline void wait(EventHub &event_hub,
			frigg::CallbackPtr<void(HelError)> callback) {
		int64_t async_id;
		HEL_CHECK(helSubmitWaitForIrq(p_handle, event_hub.getHandle(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(), &async_id));
	}

	inline void subscribe(EventHub &event_hub,
			frigg::CallbackPtr<void(HelError)> callback) {
		int64_t async_id;
		HEL_CHECK(helSubscribeIrq(p_handle, event_hub.getHandle(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(), &async_id));
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

	explicit inline Directory(HelHandle handle) : p_handle(handle) { }
	
	inline Directory(Directory &&other)
	: Directory() {
		swap(*this, other);
	}

	inline Directory(const Directory &other) = delete;

	inline ~Directory() {
		reset();
	}
	
	inline Directory &operator= (Directory other) {
		swap(*this, other);
		return *this;
	}

	void reset() {
		if(p_handle != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(p_handle));
		p_handle = kHelNullHandle;
	}

	friend inline void swap(Directory &a, Directory &b) {
		using frigg::swap;
		swap(a.p_handle, b.p_handle);
	}

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

// --------------------------------------------------------
// EventHub implementation
// --------------------------------------------------------

void EventHub::waitForSendString(int64_t async_id, HelError &error) {
	HelEvent event = waitForEvent(async_id);
	error = event.error;
}
void EventHub::waitForSendDescriptor(int64_t async_id, HelError &error) {
	HelEvent event = waitForEvent(async_id);
	error = event.error;
}
void EventHub::waitForRecvString(int64_t async_id, HelError &error, size_t &length) {
	HelEvent event = waitForEvent(async_id);
	error = event.error;
	length = event.length;
}
void EventHub::waitForRecvDescriptor(int64_t async_id, HelError &error, HelHandle &handle) {
	HelEvent event = waitForEvent(async_id);
	error = event.error;
	handle = event.handle;
}
void EventHub::waitForConnect(int64_t async_id, HelError &error, Pipe &pipe) {
	HelEvent event = waitForEvent(async_id);
	error = event.error;
	pipe = Pipe(event.handle);
}

} // namespace helx

#endif // HELX_HPP

