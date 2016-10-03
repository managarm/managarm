
#ifndef HELX_HPP
#define HELX_HPP

#include <frigg/cxx-support.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/callback.hpp>

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
typedef void (*ObserveFunction) (void *, HelError);
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
			case kHelEventObserve: {
				auto function = (ObserveFunction)evt.submitFunction;
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
		HelEvent event;
		HEL_CHECK(helWaitForCertainEvent(p_handle, async_id, &event,
				kHelWaitInfinite));
		assert(event.asyncId == async_id);
		return event;
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

	inline Pipe() : _handle(kHelNullHandle) { }

	inline Pipe(Pipe &&other)
	: Pipe() {
		swap(*this, other);
	}

	inline Pipe(const Pipe &other) = delete;

	explicit inline Pipe(HelHandle handle) : _handle(handle) { }

	inline ~Pipe() {
		if(_handle != kHelNullHandle)
			HEL_CHECK(helCloseDescriptor(_handle));
	}

	inline Pipe &operator= (Pipe other) {
		swap(*this, other);
		return *this;
	}

	inline void release() {
		_handle = kHelNullHandle;
	}

	friend inline void swap(Pipe &a, Pipe &b) {
		using frigg::swap;
		swap(a._handle, b._handle);
	}

	inline HelHandle getHandle() {
		return _handle;
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

	inline void sendStringSync(const void *buffer, size_t length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			uint32_t flags, HelError &error) {
		int64_t async_id;
		HEL_CHECK(helSubmitSendString(_handle, event_hub.getHandle(),
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
	
	inline void sendDescriptorSync(HelHandle send_handle,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			uint32_t flags, HelError &error) {
		int64_t async_id;
		HEL_CHECK(helSubmitSendDescriptor(_handle, event_hub.getHandle(),
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

	[[ deprecated ]] inline HelError recvString(void *buffer, size_t max_length,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError, int64_t, int64_t, size_t)> callback,
			uint32_t flags) {
		int64_t async_id;
		return helSubmitRecvString(_handle, event_hub.getHandle(),
				(uint8_t *)buffer, max_length, msg_request, msg_seq,
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
				flags, &async_id);
	}
	[[ deprecated ]] inline HelError recvStringToRing(HelHandle ring_handle,
			EventHub &event_hub, int64_t msg_request, int64_t msg_seq,
			frigg::CallbackPtr<void(HelError, int64_t, int64_t, size_t, size_t, size_t)> callback,
			uint32_t flags) {
		int64_t async_id;
		return helSubmitRecvStringToRing(_handle, event_hub.getHandle(),
				ring_handle, msg_request, msg_seq,
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
				flags, &async_id);
	}
	[[ deprecated ]] inline HelError recvStringReq(void *buffer, size_t max_length,
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
		HelError submit_error = helSubmitRecvString(_handle, event_hub.getHandle(),
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
		HEL_CHECK(helSubmitRecvDescriptor(_handle, event_hub.getHandle(),
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
		HEL_CHECK(helSubmitRecvDescriptor(_handle, event_hub.getHandle(),
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
	HelHandle _handle;
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

} // namespace helx

#endif // HELX_HPP

