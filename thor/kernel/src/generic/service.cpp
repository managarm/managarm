
#include "kernel.hpp"

#include <frigg/callback.hpp>

#include <xuniverse.frigg_pb.hpp>

namespace thor {

// TODO: move this to a header file
extern frigg::LazyInitializer<frigg::SharedPtr<Universe>> rootUniverse;

void serviceSend(frigg::SharedPtr<Channel> channel, const void *buffer, size_t length,
		frigg::UnsafePtr<EventHub> hub, frigg::CallbackPtr<void(AsyncEvent)> callback) {
	frigg::UniqueMemory<KernelAlloc> kernel_buffer(*kernelAlloc, length);
	memcpy(kernel_buffer.data(), buffer, length);

	auto send = frigg::makeShared<AsyncSendString>(*kernelAlloc,
			PostEventCompleter(hub.toWeak(), allocAsyncId(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject()),
			0, 0);
	send->flags = Channel::kFlagResponse;
	send->kernelBuffer = frigg::move(kernel_buffer);

	Error error;
	{
		Channel::Guard channel_guard(&channel->lock);
		error = channel->sendString(channel_guard, send);
	}
	assert(error == kErrSuccess);
}

void serviceSendDescriptor(frigg::SharedPtr<Channel> channel, AnyDescriptor descriptor,
		frigg::UnsafePtr<EventHub> hub, frigg::CallbackPtr<void(AsyncEvent)> callback) {
	auto send = frigg::makeShared<AsyncSendDescriptor>(*kernelAlloc,
			PostEventCompleter(hub.toWeak(), allocAsyncId(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject()),
			0, 0);
	send->flags = Channel::kFlagResponse;
	send->descriptor = frigg::move(descriptor);

	Error error;
	{
		Channel::Guard channel_guard(&channel->lock);
		error = channel->sendDescriptor(channel_guard, send);
	}
	assert(error == kErrSuccess);
}

void serviceRecv(frigg::SharedPtr<Channel> channel, void *buffer, size_t max_length,
		frigg::UnsafePtr<EventHub> hub, frigg::CallbackPtr<void(AsyncEvent)> callback) {
	frigg::UnsafePtr<Thread> this_thread = getCurrentThread();
	frigg::UnsafePtr<AddressSpace> this_space = this_thread->getAddressSpace();

	auto recv = frigg::makeShared<AsyncRecvString>(*kernelAlloc,
			PostEventCompleter(hub.toWeak(), allocAsyncId(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject()),
			AsyncRecvString::kTypeNormal, -1, 0);
	recv->flags = Channel::kFlagRequest;
	recv->spaceLock = ForeignSpaceLock::acquire(this_space.toShared(), buffer, max_length);

	Error error;
	{
		Channel::Guard channel_guard(&channel->lock);
		error = channel->submitRecvString(channel_guard, recv);
	}
	assert(error == kErrSuccess);
}

frigg::SharedPtr<Channel> rootRecvChannel() {
	return frigg::SharedPtr<Channel>(*rootUniverse,
			&(*rootUniverse)->superiorRecvChannel());
}

frigg::SharedPtr<Channel> rootSendChannel() {
	return frigg::SharedPtr<Channel>(*rootUniverse,
			&(*rootUniverse)->superiorSendChannel());
}

void serviceMain() {
	namespace proto = managarm::xuniverse;

	disableInts();
	frigg::infoLogger() << "In service thread" << frigg::endLog;

	struct AllocatorPolicy {
		uintptr_t map(size_t length) {
			assert((length % 0x1000) == 0);
			
			frigg::UnsafePtr<Thread> this_thread = getCurrentThread();
			frigg::UnsafePtr<AddressSpace> this_space = this_thread->getAddressSpace();

			auto memory = frigg::makeShared<Memory>(*kernelAlloc, AllocatedMemory(length));

			VirtualAddr actual_address;
			{
				AddressSpace::Guard space_guard(&this_space->lock);
				this_space->map(space_guard, memory, 0, 0, length,
						AddressSpace::kMapPreferTop | AddressSpace::kMapReadWrite,
						&actual_address);
			}

			return actual_address;
		}

		void unmap(uintptr_t address, size_t length) {
			assert(!"Unmapping memory is not implemented here");
		}
	};

	typedef frigg::SlabAllocator<AllocatorPolicy, frigg::TicketLock> ServiceAllocator;
	AllocatorPolicy policy;
	ServiceAllocator allocator(policy);

	struct GetProfileClosure {
		GetProfileClosure(ServiceAllocator &allocator,
				frigg::SharedPtr<EventHub> hub,
				proto::CntRequest<ServiceAllocator> request,
				uint64_t request_id)
		: _allocator(allocator), _hub(frigg::move(hub)), _req(frigg::move(request)),
				_requestId(request_id), _buffer(allocator) { }

		void operator() () {
			proto::SvrResponse<ServiceAllocator> resp(_allocator);
			resp.set_profile(frigg::String<ServiceAllocator>(_allocator, "minimal"));
			resp.set_error(proto::Errors::SUCCESS);
			resp.SerializeToString(&_buffer);
			serviceSend(rootSendChannel(), _buffer.data(), _buffer.size(),
					_hub, CALLBACK_MEMBER(this, &GetProfileClosure::onSend));
		}

	private:
		void onSend(AsyncEvent event) {
			assert(event.error == kErrSuccess);
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<EventHub> _hub;
		proto::CntRequest<ServiceAllocator> _req;
		uint64_t _requestId;

		frigg::String<ServiceAllocator> _buffer;
	};

	struct GetServerClosure {
		GetServerClosure(ServiceAllocator &allocator,
				frigg::SharedPtr<EventHub> hub,
				proto::CntRequest<ServiceAllocator> request,
				uint64_t request_id)
		: _allocator(allocator), _hub(frigg::move(hub)), _req(frigg::move(request)),
				_requestId(request_id), _buffer(allocator) { }

		void operator() () {
			if(_req.server() == "fs") {
				auto pipe = frigg::makeShared<FullPipe>(*kernelAlloc);

				// we increment the owning reference count twice here. it is decremented
				// each time one of the EndpointRwControl references is decremented to zero.
				pipe.control().increment();
				pipe.control().increment();
				frigg::SharedPtr<Endpoint, EndpointRwControl> local_end(frigg::adoptShared,
						&pipe->endpoint(0),
						EndpointRwControl(&pipe->endpoint(0), pipe.control().counter()));
				frigg::SharedPtr<Endpoint, EndpointRwControl> remote_end(frigg::adoptShared,
						&pipe->endpoint(1),
						EndpointRwControl(&pipe->endpoint(1), pipe.control().counter()));

				serviceSendDescriptor(rootSendChannel(),
						EndpointDescriptor(frigg::move(remote_end)),
						_hub, CALLBACK_MEMBER(this, &GetServerClosure::onSend));
			}else{
				assert(!"Unexpected server for GET_SERVER");
			}
		}

	private:
		void onSend(AsyncEvent event) {
			assert(event.error == kErrSuccess);
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<EventHub> _hub;
		proto::CntRequest<ServiceAllocator> _req;
		uint64_t _requestId;

		frigg::String<ServiceAllocator> _buffer;
	};

	struct RequestClosure {
		RequestClosure(ServiceAllocator &allocator,
				frigg::SharedPtr<EventHub> hub)
		: _allocator(allocator), _hub(frigg::move(hub)) { }

		void operator() () {
			serviceRecv(rootRecvChannel(), _buffer, 128,
					_hub, CALLBACK_MEMBER(this, &RequestClosure::onReceive));
		}
	
	private:
		void onReceive(AsyncEvent event) {
			assert(event.error == kErrSuccess);

			proto::CntRequest<ServiceAllocator> request(_allocator);
			request.ParseFromArray(_buffer, event.length);
		
			if(request.req_type() == proto::CntReqType::GET_PROFILE) {
				auto closure = frigg::construct<GetProfileClosure>(_allocator, _allocator,
						_hub, frigg::move(request), event.msgRequest);
				(*closure)();
			}else if(request.req_type() == proto::CntReqType::GET_SERVER) {
				auto closure = frigg::construct<GetServerClosure>(_allocator, _allocator,
						_hub, frigg::move(request), event.msgRequest);
				(*closure)();
			}else{
				assert(!"Illegal request type");
			}

			(*this)();
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<EventHub> _hub;

		uint8_t _buffer[128];
	};

	frigg::SharedPtr<EventHub> hub = frigg::makeShared<EventHub>(*kernelAlloc);

	auto closure = frigg::construct<RequestClosure>(allocator, allocator, hub);
	(*closure)();

	while(true) {
		struct NullAllocator {
			void free(void *) { }
		};
		NullAllocator null_allocator;
			
		frigg::UnsafePtr<Thread> this_thread = getCurrentThread();
		frigg::SharedBlock<AsyncWaitForEvent, NullAllocator> block(null_allocator,
				ReturnFromForkCompleter(this_thread.toWeak()), -1);
		frigg::SharedPtr<AsyncWaitForEvent> wait(frigg::adoptShared, &block);

		Thread::blockCurrent([&] () {
			EventHub::Guard hub_guard(&hub->lock);
			hub->submitWaitForEvent(hub_guard, wait);
		});

		frigg::CallbackPtr<void(AsyncEvent)> cb((void *)wait->event.submitInfo.submitObject,
				(void (*) (void *, AsyncEvent))wait->event.submitInfo.submitFunction);
		cb(wait->event);
	}
}

void runService() {
	auto space = frigg::makeShared<AddressSpace>(*kernelAlloc,
			kernelSpace->cloneFromKernelSpace());
	space->setupDefaultMappings();

	// allocate and map memory for the user mode stack
	size_t stack_size = 0x10000;
	auto stack_memory = frigg::makeShared<Memory>(*kernelAlloc,
			AllocatedMemory(stack_size));

	VirtualAddr stack_base;
	{
		AddressSpace::Guard space_guard(&space->lock);
		space->map(space_guard, stack_memory, 0, 0, stack_size,
				AddressSpace::kMapPreferTop | AddressSpace::kMapReadWrite
				| AddressSpace::kMapPopulate, &stack_base);
	}
	thorRtInvalidateSpace();

	// create a thread for the module
	auto thread = frigg::makeShared<Thread>(*kernelAlloc, frigg::SharedPtr<Universe>(),
			frigg::move(space), frigg::SharedPtr<RdFolder>());
	thread->flags |= Thread::kFlagExclusive | Thread::kFlagTrapsAreFatal;
	
	thread->image.initSystemVAbi((uintptr_t)&serviceMain,
			(uintptr_t)thread->kernelStack.base(), true);
//			stack_base + stack_size, true);

	// see helCreateThread for the reasoning here
	thread.control().increment();
	thread.control().increment();

	ScheduleGuard schedule_guard(scheduleLock.get());
	enqueueInSchedule(schedule_guard, frigg::move(thread));
}

} // namespace thor

