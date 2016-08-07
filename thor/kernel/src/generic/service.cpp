
#include "kernel.hpp"

#include <xuniverse.frigg_pb.hpp>

namespace thor {

// TODO: move this to a header file
extern frigg::LazyInitializer<frigg::SharedPtr<Universe>> rootUniverse;

void serviceSend(frigg::SharedPtr<Channel> channel, const void *buffer, size_t length) {
	frigg::UnsafePtr<Thread> this_thread = getCurrentThread();
	frigg::UnsafePtr<AddressSpace> this_space = this_thread->getAddressSpace();
	
	frigg::UniqueMemory<KernelAlloc> kernel_buffer(*kernelAlloc, length);
	memcpy(kernel_buffer.data(), buffer, length);

	auto send = frigg::makeShared<AsyncSendString>(*kernelAlloc,
			ReturnFromForkCompleter(this_thread.toWeak()),
			0, 0);
	send->flags = Channel::kFlagResponse;
	send->kernelBuffer = frigg::move(kernel_buffer);

	forkAndSchedule([&] () {
		Channel::Guard channel_guard(&channel->lock);
		Error error = channel->sendString(channel_guard, frigg::move(send));
		assert(error == kErrSuccess);
	});

	// FIXME: check for async errors
}

template<typename T>
void serviceSerializeTo(frigg::SharedPtr<Channel> channel, T &message) {
	frigg::String<KernelAlloc> serialized(*kernelAlloc);
	message.SerializeToString(&serialized);
	serviceSend(channel, serialized.data(), serialized.size());
}

void serviceSendDescriptor(frigg::SharedPtr<Channel> channel, AnyDescriptor descriptor) {
	frigg::UnsafePtr<Thread> this_thread = getCurrentThread();

	auto send = frigg::makeShared<AsyncSendDescriptor>(*kernelAlloc,
			ReturnFromForkCompleter(this_thread.toWeak()),
			0, 0);
	send->flags = Channel::kFlagResponse;
	send->descriptor = frigg::move(descriptor);

	forkAndSchedule([&] () {
		Channel::Guard channel_guard(&channel->lock);
		Error error = channel->sendDescriptor(channel_guard, frigg::move(send));
		assert(error == kErrSuccess);
	});

	// FIXME: check for async errors
}

void serviceRecv(frigg::SharedPtr<Channel> channel, void *buffer, size_t max_length,
		size_t &actual_length) {
	frigg::UnsafePtr<Thread> this_thread = getCurrentThread();
	frigg::UnsafePtr<AddressSpace> this_space = this_thread->getAddressSpace();

	auto recv = frigg::makeShared<AsyncRecvString>(*kernelAlloc,
			ReturnFromForkCompleter(this_thread.toWeak()),
			AsyncRecvString::kTypeNormal, -1, 0);
	recv->flags = Channel::kFlagRequest;
	recv->spaceLock = ForeignSpaceLock::acquire(this_space.toShared(), buffer, max_length);

	forkAndSchedule([&] () {
		Channel::Guard channel_guard(&channel->lock);
		Error error = channel->submitRecvString(channel_guard, recv);
		assert(error == kErrSuccess);
	});

	// FIXME: check for async errors
	actual_length = recv->length;
}

template<typename T>
void serviceParseFrom(frigg::SharedPtr<Channel> channel, T &message) {
	uint8_t buffer[128];
	size_t length;
	serviceRecv(channel, buffer, 128, length);
	message.ParseFromArray(buffer, length);
}

void serviceMain() {
	namespace proto = managarm::xuniverse;

	disableInts();

	frigg::UnsafePtr<Thread> this_thread = getCurrentThread();
	frigg::UnsafePtr<AddressSpace> this_space = this_thread->getAddressSpace();

	frigg::infoLogger() << "In service thread" << frigg::endLog;

	auto recv_channel = frigg::SharedPtr<Channel>(*rootUniverse,
			&(*rootUniverse)->superiorRecvChannel());
	auto send_channel = frigg::SharedPtr<Channel>(*rootUniverse,
			&(*rootUniverse)->superiorSendChannel());

	while(true) {
		proto::CntRequest<KernelAlloc> req(*kernelAlloc);
		serviceParseFrom(recv_channel, req);
		if(req.req_type() == proto::CntReqType::GET_PROFILE) {
			proto::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(proto::Errors::SUCCESS);
			resp.set_profile(frigg::String<KernelAlloc>(*kernelAlloc, "minimal"));
			serviceSerializeTo(send_channel, resp);
		}else if(req.req_type() == proto::CntReqType::GET_SERVER
				&& req.server() == "fs") {
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

			serviceSendDescriptor(send_channel, EndpointDescriptor(frigg::move(remote_end)));
		}else{
			assert(!"Illegal request type");
		}
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
//			(uintptr_t)thread->kernelStack.base(), true);
			stack_base + stack_size, true);

	// see helCreateThread for the reasoning here
	thread.control().increment();
	thread.control().increment();

	ScheduleGuard schedule_guard(scheduleLock.get());
	enqueueInSchedule(schedule_guard, frigg::move(thread));
}

} // namespace thor

