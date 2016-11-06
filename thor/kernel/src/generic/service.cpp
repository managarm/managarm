
#include "kernel.hpp"
#include "module.hpp"

#include <frigg/callback.hpp>

#include <posix.frigg_pb.hpp>
#include <fs.frigg_pb.hpp>

namespace thor {

// TODO: move this to a header file
extern frigg::LazyInitializer<frigg::SharedPtr<Endpoint, EndpointRwControl>> initrdServer;

namespace posix = managarm::posix;
namespace fs = managarm::fs;

void serviceSend(frigg::SharedPtr<Channel> channel, int64_t req_id, int64_t seq_id,
		const void *buffer, size_t length, frigg::UnsafePtr<EventHub> hub,
		frigg::CallbackPtr<void(AsyncEvent)> callback) {
	frigg::UniqueMemory<KernelAlloc> kernel_buffer(*kernelAlloc, length);
	memcpy(kernel_buffer.data(), buffer, length);

	auto send = frigg::makeShared<AsyncSendString>(*kernelAlloc,
			PostEventCompleter(hub.toWeak(), allocAsyncId(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject()),
			req_id, seq_id);
	send->flags = Channel::kFlagResponse;
	send->kernelBuffer = frigg::move(kernel_buffer);

	Error error;
	{
		Channel::Guard channel_guard(&channel->lock);
		error = channel->sendString(channel_guard, send);
	}
	assert(error == kErrSuccess);
}

void serviceSendDescriptor(frigg::SharedPtr<Channel> channel, int64_t req_id, int64_t seq_id,
		AnyDescriptor descriptor, frigg::UnsafePtr<EventHub> hub,
		frigg::CallbackPtr<void(AsyncEvent)> callback) {
	auto send = frigg::makeShared<AsyncSendDescriptor>(*kernelAlloc,
			PostEventCompleter(hub.toWeak(), allocAsyncId(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject()),
			req_id, seq_id);
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
	recv->spaceLock = ForeignSpaceAccessor::acquire(this_space.toShared(), buffer, max_length);

	Error error;
	{
		Channel::Guard channel_guard(&channel->lock);
		error = channel->submitRecvString(channel_guard, recv);
	}
	assert(error == kErrSuccess);
}

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
		(void)address;
		(void)length;
		assert(!"Unmapping memory is not implemented here");
		__builtin_trap();
	}
};

typedef frigg::SlabAllocator<AllocatorPolicy, frigg::TicketLock> ServiceAllocator;

namespace initrd {
	struct OpenFile {
		OpenFile(Module *module)
		: module(module), offset(0) { }

		Module *module;
		size_t offset;
	};

	// ----------------------------------------------------
	// initrd file handling.
	// ----------------------------------------------------

	struct FileConnection {
		FileConnection(ServiceAllocator &, OpenFile *file,
				frigg::SharedPtr<Endpoint, EndpointRwControl> endpoint)
		: file(file), endpoint(frigg::move(endpoint)) { }

		OpenFile *file;
		frigg::SharedPtr<Endpoint, EndpointRwControl> endpoint;
	};
	
	struct SeekClosure {
		SeekClosure(ServiceAllocator &allocator, frigg::SharedPtr<FileConnection> connection,
				frigg::SharedPtr<EventHub> hub, fs::CntRequest<ServiceAllocator> request,
				uint64_t request_id)
		: _allocator(allocator), _connection(frigg::move(connection)), _hub(frigg::move(hub)),
				_req(frigg::move(request)), _requestId(request_id), _buffer(allocator) { }

		void operator() () {
			_connection->file->offset = _req.rel_offset();

			fs::SvrResponse<ServiceAllocator> resp(_allocator);
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.SerializeToString(&_buffer);
			serviceSend(Endpoint::writeChannel(_connection->endpoint), _requestId, 0,
					_buffer.data(), _buffer.size(), _hub,
					CALLBACK_MEMBER(this, &SeekClosure::onSend));
		}

	private:
		void onSend(AsyncEvent event) {
			assert(event.error == kErrSuccess);
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<FileConnection> _connection;
		frigg::SharedPtr<EventHub> _hub;
		fs::CntRequest<ServiceAllocator> _req;
		uint64_t _requestId;

		frigg::String<ServiceAllocator> _buffer;
	};
	
	struct ReadClosure {
		ReadClosure(ServiceAllocator &allocator, frigg::SharedPtr<FileConnection> connection,
				frigg::SharedPtr<EventHub> hub, fs::CntRequest<ServiceAllocator> request,
				uint64_t request_id)
		: _allocator(allocator), _connection(frigg::move(connection)), _hub(frigg::move(hub)),
				_req(frigg::move(request)), _requestId(request_id),
				_buffer(allocator), _payload(allocator) { }

		void operator() () {
			size_t remaining = _connection->file->module->length - _connection->file->offset;
			assert(remaining);

			_payload.resize(frigg::min(size_t(_req.size()), remaining));
			void *src = physicalToVirtual(_connection->file->module->physical
					+ _connection->file->offset);
			memcpy(_payload.data(), src, _payload.size());

			fs::SvrResponse<ServiceAllocator> resp(_allocator);
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.SerializeToString(&_buffer);
			serviceSend(Endpoint::writeChannel(_connection->endpoint), _requestId, 0,
					_buffer.data(), _buffer.size(), _hub,
					CALLBACK_MEMBER(this, &ReadClosure::onSendResp));
		}

	private:
		void onSendResp(AsyncEvent event) {
			assert(event.error == kErrSuccess);
			
			serviceSend(Endpoint::writeChannel(_connection->endpoint), _requestId, 1,
					_payload.data(), _payload.size(), _hub,
					CALLBACK_MEMBER(this, &ReadClosure::onSendData));
		}
		
		void onSendData(AsyncEvent event) {
			assert(event.error == kErrSuccess);
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<FileConnection> _connection;
		frigg::SharedPtr<EventHub> _hub;
		fs::CntRequest<ServiceAllocator> _req;
		uint64_t _requestId;

		frigg::String<ServiceAllocator> _buffer;
		frigg::String<ServiceAllocator> _payload;
	};
	
	struct MapClosure {
		MapClosure(ServiceAllocator &allocator, frigg::SharedPtr<FileConnection> connection,
				frigg::SharedPtr<EventHub> hub, fs::CntRequest<ServiceAllocator> request,
				uint64_t request_id)
		: _allocator(allocator), _connection(frigg::move(connection)), _hub(frigg::move(hub)),
				_req(frigg::move(request)), _requestId(request_id), _buffer(allocator) { }

		void operator() () {
			fs::SvrResponse<ServiceAllocator> resp(_allocator);
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.SerializeToString(&_buffer);
			serviceSend(Endpoint::writeChannel(_connection->endpoint), _requestId, 0,
					_buffer.data(), _buffer.size(), _hub,
					CALLBACK_MEMBER(this, &MapClosure::onSendResp));
		}

	private:
		void onSendResp(AsyncEvent event) {
			assert(event.error == kErrSuccess);
			
			size_t virt_length = _connection->file->module->length;
			if(virt_length % kPageSize)
				virt_length += kPageSize - (virt_length % kPageSize);

			auto memory = frigg::makeShared<Memory>(*kernelAlloc,
					HardwareMemory(_connection->file->module->physical, virt_length));
			serviceSendDescriptor(Endpoint::writeChannel(_connection->endpoint), _requestId, 1,
					MemoryAccessDescriptor(frigg::move(memory)),
					_hub, CALLBACK_MEMBER(this, &MapClosure::onSendHandle));
		}
		
		void onSendHandle(AsyncEvent event) {
			assert(event.error == kErrSuccess);
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<FileConnection> _connection;
		frigg::SharedPtr<EventHub> _hub;
		fs::CntRequest<ServiceAllocator> _req;
		uint64_t _requestId;

		frigg::String<ServiceAllocator> _buffer;
	};

	struct FileRequestClosure {
		FileRequestClosure(ServiceAllocator &allocator,
				frigg::SharedPtr<FileConnection> connection,
				frigg::SharedPtr<EventHub> hub)
		: _allocator(allocator), _connection(frigg::move(connection)), _hub(frigg::move(hub)) { }

		void operator() () {
			serviceRecv(Endpoint::readChannel(_connection->endpoint), _buffer, 128,
					_hub, CALLBACK_MEMBER(this, &FileRequestClosure::onReceive));
		}

	private:
		void onReceive(AsyncEvent event) {
			if(event.error == kErrClosedRemotely)
				return;
			assert(event.error == kErrSuccess);

			fs::CntRequest<ServiceAllocator> request(_allocator);
			request.ParseFromArray(_buffer, event.length);

/*			if(request.req_type() == managarm::fs::CntReqType::FSTAT) {
				auto closure = frigg::construct<StatClosure>(*allocator,
						*this, msg_request, frigg::move(request));
				(*closure)();
			}else*/ if(request.req_type() == managarm::fs::CntReqType::READ) {
				auto closure = frigg::construct<ReadClosure>(_allocator, _allocator,
						_connection, _hub, frigg::move(request), event.msgRequest);
				(*closure)();
			}else if(request.req_type() == managarm::fs::CntReqType::SEEK_ABS) {
				auto closure = frigg::construct<SeekClosure>(_allocator, _allocator,
						_connection, _hub, frigg::move(request), event.msgRequest);
				(*closure)();
			}else if(request.req_type() == managarm::fs::CntReqType::MMAP) {
				auto closure = frigg::construct<MapClosure>(_allocator, _allocator,
						_connection, _hub, frigg::move(request), event.msgRequest);
				(*closure)();
			}else{
				frigg::panicLogger() << "Illegal request type" << frigg::endLog;
			}

			(*this)();
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<FileConnection> _connection;
		frigg::SharedPtr<EventHub> _hub;

		uint8_t _buffer[128];
	};

	// ----------------------------------------------------
	// POSIX server.
	// ----------------------------------------------------

	struct ServerConnection {
		ServerConnection(ServiceAllocator &,
				frigg::SharedPtr<Endpoint, EndpointRwControl> endpoint)
		: endpoint(frigg::move(endpoint)) { }

		frigg::SharedPtr<Endpoint, EndpointRwControl> endpoint;
	};

	struct OpenClosure {
		OpenClosure(ServiceAllocator &allocator, frigg::SharedPtr<ServerConnection> connection,
				frigg::SharedPtr<EventHub> hub, posix::ClientRequest<ServiceAllocator> request,
				uint64_t request_id)
		: _allocator(allocator), _connection(frigg::move(connection)), _hub(frigg::move(hub)),
				_req(frigg::move(request)), _requestId(request_id), _buffer(allocator) { }

		void operator() () {
			frigg::infoLogger() << "initrd: '" <<  _req.path() << "' requested." << frigg::endLog;
			posix::ServerResponse<ServiceAllocator> resp(_allocator);
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.SerializeToString(&_buffer);
			serviceSend(Endpoint::writeChannel(_connection->endpoint), _requestId, 0,
					_buffer.data(), _buffer.size(), _hub,
					CALLBACK_MEMBER(this, &OpenClosure::onSendResp));
		}

	private:
		void onSendResp(AsyncEvent event) {
			assert(event.error == kErrSuccess);
			
			// TODO: this should not be handled here!
			Module *module = getModule(_req.path());
			assert(module);

			// we increment the owning reference count twice here. it is decremented
			// each time one of the EndpointRwControl references is decremented to zero.
			auto pipe = frigg::makeShared<FullPipe>(*kernelAlloc);
			pipe.control().increment();
			pipe.control().increment();
			frigg::SharedPtr<Endpoint, EndpointRwControl> file_server(frigg::adoptShared,
					&pipe->endpoint(0),
					EndpointRwControl(&pipe->endpoint(0), pipe.control().counter()));
			frigg::SharedPtr<Endpoint, EndpointRwControl> file_client(frigg::adoptShared,
					&pipe->endpoint(1),
					EndpointRwControl(&pipe->endpoint(1), pipe.control().counter()));

			auto connection = frigg::makeShared<initrd::FileConnection>(_allocator, _allocator,
					frigg::construct<OpenFile>(_allocator, module), frigg::move(file_server));
			auto closure = frigg::construct<initrd::FileRequestClosure>(_allocator, _allocator,
					frigg::move(connection), _hub);
			(*closure)();
			
			serviceSendDescriptor(Endpoint::writeChannel(_connection->endpoint), _requestId, 1,
					EndpointDescriptor(frigg::move(file_client)),
					_hub, CALLBACK_MEMBER(this, &OpenClosure::onSendHandle));
		}

		void onSendHandle(AsyncEvent event) {
			assert(event.error == kErrSuccess);
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<ServerConnection> _connection;
		frigg::SharedPtr<EventHub> _hub;
		posix::ClientRequest<ServiceAllocator> _req;
		uint64_t _requestId;

		frigg::String<ServiceAllocator> _buffer;
	};

	struct ServerRequestClosure {
		ServerRequestClosure(ServiceAllocator &allocator,
				frigg::SharedPtr<ServerConnection> connection,
				frigg::SharedPtr<EventHub> hub)
		: _allocator(allocator), _connection(frigg::move(connection)), _hub(frigg::move(hub)) { }

		void operator() () {
			serviceRecv(Endpoint::readChannel(_connection->endpoint), _buffer, 128,
					_hub, CALLBACK_MEMBER(this, &ServerRequestClosure::onReceive));
		}

	private:
		void onReceive(AsyncEvent event) {
			assert(event.error == kErrSuccess);

			posix::ClientRequest<ServiceAllocator> request(_allocator);
			request.ParseFromArray(_buffer, event.length);

			if(request.request_type() == managarm::posix::ClientRequestType::OPEN) {
				auto closure = frigg::construct<OpenClosure>(_allocator, _allocator,
						_connection, _hub, frigg::move(request), event.msgRequest);
				(*closure)();
			}else{
				frigg::panicLogger() << "Illegal request type" << frigg::endLog;
			}

			(*this)();
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<ServerConnection> _connection;
		frigg::SharedPtr<EventHub> _hub;

		uint8_t _buffer[128];
	};
}

void serviceMain() {
	disableInts();
	frigg::infoLogger() << "In service thread" << frigg::endLog;
	
	AllocatorPolicy policy;
	ServiceAllocator allocator(policy);

	frigg::SharedPtr<EventHub> hub = frigg::makeShared<EventHub>(*kernelAlloc);

	// start the initrd server.
	auto connection = frigg::makeShared<initrd::ServerConnection>(allocator, allocator,
			frigg::move(*initrdServer));
	auto closure = frigg::construct<initrd::ServerRequestClosure>(allocator, allocator,
			frigg::move(connection), hub);
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
		{
			EventHub::Guard hub_guard(&hub->lock);
			hub->submitWaitForEvent(hub_guard, wait);
		}

		Thread::blockCurrentWhile([&] {
			return !wait->isComplete.load(std::memory_order_acquire);
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
			frigg::move(space));
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

