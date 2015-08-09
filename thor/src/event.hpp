
namespace thor {

struct SubmitInfo {
	SubmitInfo(int64_t submit_id, uintptr_t submit_function,
			uintptr_t submit_object);
	
	int64_t submitId;
	uintptr_t submitFunction;
	uintptr_t submitObject;
};

class EventHub {
public:
	struct Event {
		enum Type {
			kTypeNone,
			kTypeRecvStringTransfer,
			kTypeRecvStringError,
			kTypeRecvDescriptor,
			kTypeAccept,
			kTypeConnect,
			kTypeIrq
		};

		Event(Type type, SubmitInfo submit_info);
		
		Type type;
		SubmitInfo submitInfo;

		// used by kTypeRecvStringError
		Error error;

		// used by kTypeRecvStringTransfer
		uint8_t *kernelBuffer;
		uint8_t *userBuffer;
		size_t length;

		// used by kTypeAccept, kTypeConnect
		SharedPtr<BiDirectionPipe, KernelAlloc> pipe;

		// used by kTypeRecvDescriptor
		AnyDescriptor descriptor;
	};

	EventHub();

	void raiseRecvStringTransferEvent(uint8_t *kernel_buffer,
			uint8_t *user_buffer, size_t length,
			SubmitInfo submit_info);
	void raiseRecvStringErrorEvent(Error error,
			SubmitInfo submit_info);
	void raiseRecvDescriptorEvent(AnyDescriptor &&descriptor,
			SubmitInfo submit_info);
	
	void raiseAcceptEvent(SharedPtr<BiDirectionPipe, KernelAlloc> &&pipe,
			SubmitInfo submit_info);
	void raiseConnectEvent(SharedPtr<BiDirectionPipe, KernelAlloc> &&pipe,
			SubmitInfo submit_info);

	void raiseIrqEvent(SubmitInfo submit_info);

	bool hasEvent();
	Event dequeueEvent();

private:
	frigg::util::LinkedList<Event, KernelAlloc> p_queue;
};

} // namespace thor

