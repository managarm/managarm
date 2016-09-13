
#include "kernel.hpp"
#include "module.hpp"

#include <frigg/callback.hpp>

#include <xuniverse.frigg_pb.hpp>
#include <fs.frigg_pb.hpp>

namespace thor {

// TODO: move this to a header file
extern frigg::LazyInitializer<frigg::SharedPtr<Universe>> rootUniverse;

namespace xuniverse = managarm::xuniverse;
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

namespace initrd {
	struct OpenFile {
		OpenFile(Module *module)
		: module(module), offset(0) { }

		Module *module;
		size_t offset;
	};

	struct Connection {
		Connection(ServiceAllocator &allocator,
				frigg::SharedPtr<Endpoint, EndpointRwControl> endpoint)
		: endpoint(frigg::move(endpoint)), fileHandles(frigg::DefaultHasher<int>(), allocator),
				nextHandle(1) { }

		frigg::SharedPtr<Endpoint, EndpointRwControl> endpoint;

		frigg::Hashmap<int, OpenFile *,
				frigg::DefaultHasher<int>, ServiceAllocator> fileHandles;
		int nextHandle;
	};

	struct OpenClosure {
		OpenClosure(ServiceAllocator &allocator, frigg::SharedPtr<Connection> connection,
				frigg::SharedPtr<EventHub> hub, fs::CntRequest<ServiceAllocator> request,
				uint64_t request_id)
		: _allocator(allocator), _connection(frigg::move(connection)), _hub(frigg::move(hub)),
				_req(frigg::move(request)), _requestId(request_id), _buffer(allocator) { }

		void operator() () {
			frigg::infoLogger() << "initrd: '" <<  _req.path() << "' requested." << frigg::endLog;
			Module *module = getModule(_req.path());
			assert(module);

			auto file = frigg::construct<OpenFile>(_allocator, module);
			int fd = _connection->nextHandle++;
			_connection->fileHandles.insert(fd, file);

			fs::SvrResponse<ServiceAllocator> resp(_allocator);
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_fd(fd);
			resp.set_file_type(managarm::fs::FileType::REGULAR);
			resp.SerializeToString(&_buffer);
			serviceSend(Endpoint::writeChannel(_connection->endpoint), _requestId, 0,
					_buffer.data(), _buffer.size(), _hub,
					CALLBACK_MEMBER(this, &OpenClosure::onSend));
		}

	private:
		void onSend(AsyncEvent event) {
			assert(event.error == kErrSuccess);
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<Connection> _connection;
		frigg::SharedPtr<EventHub> _hub;
		fs::CntRequest<ServiceAllocator> _req;
		uint64_t _requestId;

		frigg::String<ServiceAllocator> _buffer;
	};
	
	struct SeekClosure {
		SeekClosure(ServiceAllocator &allocator, frigg::SharedPtr<Connection> connection,
				frigg::SharedPtr<EventHub> hub, fs::CntRequest<ServiceAllocator> request,
				uint64_t request_id)
		: _allocator(allocator), _connection(frigg::move(connection)), _hub(frigg::move(hub)),
				_req(frigg::move(request)), _requestId(request_id), _buffer(allocator) { }

		void operator() () {
			auto file = _connection->fileHandles.get(_req.fd());
			assert(file);
			(*file)->offset = _req.rel_offset();

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
		frigg::SharedPtr<Connection> _connection;
		frigg::SharedPtr<EventHub> _hub;
		fs::CntRequest<ServiceAllocator> _req;
		uint64_t _requestId;

		frigg::String<ServiceAllocator> _buffer;
	};
	
	struct ReadClosure {
		ReadClosure(ServiceAllocator &allocator, frigg::SharedPtr<Connection> connection,
				frigg::SharedPtr<EventHub> hub, fs::CntRequest<ServiceAllocator> request,
				uint64_t request_id)
		: _allocator(allocator), _connection(frigg::move(connection)), _hub(frigg::move(hub)),
				_req(frigg::move(request)), _requestId(request_id),
				_buffer(allocator), _payload(allocator) { }

		void operator() () {
			auto file = _connection->fileHandles.get(_req.fd());
			assert(file);

			size_t remaining = (*file)->module->length - (*file)->offset;
			assert(remaining);

			_payload.resize(frigg::min(size_t(_req.size()), remaining));
			void *src = physicalToVirtual((*file)->module->physical + (*file)->offset);
			memcpy(_payload.data(), src, _payload.size());

			fs::SvrResponse<ServiceAllocator> resp(_allocator);
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.SerializeToString(&_buffer);
			serviceSend(Endpoint::writeChannel(_connection->endpoint), _requestId, 0,
					_buffer.data(), _buffer.size(), _hub,
					CALLBACK_MEMBER(this, &ReadClosure::onSendBuffer));
		}

	private:
		void onSendBuffer(AsyncEvent event) {
			assert(event.error == kErrSuccess);
			
			serviceSend(Endpoint::writeChannel(_connection->endpoint), _requestId, 1,
					_payload.data(), _payload.size(), _hub,
					CALLBACK_MEMBER(this, &ReadClosure::onSendPayload));
		}
		
		void onSendPayload(AsyncEvent event) {
			assert(event.error == kErrSuccess);
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<Connection> _connection;
		frigg::SharedPtr<EventHub> _hub;
		fs::CntRequest<ServiceAllocator> _req;
		uint64_t _requestId;

		frigg::String<ServiceAllocator> _buffer;
		frigg::String<ServiceAllocator> _payload;
	};
	
	struct MapClosure {
		MapClosure(ServiceAllocator &allocator, frigg::SharedPtr<Connection> connection,
				frigg::SharedPtr<EventHub> hub, fs::CntRequest<ServiceAllocator> request,
				uint64_t request_id)
		: _allocator(allocator), _connection(frigg::move(connection)), _hub(frigg::move(hub)),
				_req(frigg::move(request)), _requestId(request_id), _buffer(allocator) { }

		void operator() () {
			auto file = _connection->fileHandles.get(_req.fd());
			assert(file);

			fs::SvrResponse<ServiceAllocator> resp(_allocator);
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.SerializeToString(&_buffer);
			serviceSend(Endpoint::writeChannel(_connection->endpoint), _requestId, 0,
					_buffer.data(), _buffer.size(), _hub,
					CALLBACK_MEMBER(this, &MapClosure::onSendBuffer));
		}

	private:
		void onSendBuffer(AsyncEvent event) {
			assert(event.error == kErrSuccess);
			
			auto file = _connection->fileHandles.get(_req.fd());
			assert(file);

			size_t virt_length = (*file)->module->length;
			if(virt_length % kPageSize)
				virt_length += kPageSize - (virt_length % kPageSize);

			auto memory = frigg::makeShared<Memory>(*kernelAlloc,
					HardwareMemory((*file)->module->physical, virt_length));
			serviceSendDescriptor(Endpoint::writeChannel(_connection->endpoint), _requestId, 1,
					MemoryAccessDescriptor(frigg::move(memory)),
					_hub, CALLBACK_MEMBER(this, &MapClosure::onSendHandle));
		}
		
		void onSendHandle(AsyncEvent event) {
			assert(event.error == kErrSuccess);
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<Connection> _connection;
		frigg::SharedPtr<EventHub> _hub;
		fs::CntRequest<ServiceAllocator> _req;
		uint64_t _requestId;

		frigg::String<ServiceAllocator> _buffer;
	};

	struct RequestClosure {
		RequestClosure(ServiceAllocator &allocator, frigg::SharedPtr<Connection> connection,
				frigg::SharedPtr<EventHub> hub)
		: _allocator(allocator), _connection(frigg::move(connection)), _hub(frigg::move(hub)) { }

		void operator() () {
			serviceRecv(Endpoint::readChannel(_connection->endpoint), _buffer, 128,
					_hub, CALLBACK_MEMBER(this, &RequestClosure::onReceive));
		}

	private:
		void onReceive(AsyncEvent event) {
			assert(event.error == kErrSuccess);

			fs::CntRequest<ServiceAllocator> request(_allocator);
			request.ParseFromArray(_buffer, event.length);

/*			if(request.req_type() == managarm::fs::CntReqType::FSTAT) {
				auto closure = frigg::construct<StatClosure>(*allocator,
						*this, msg_request, frigg::move(request));
				(*closure)();
			}else*/ if(request.req_type() == managarm::fs::CntReqType::OPEN) {
				auto closure = frigg::construct<OpenClosure>(_allocator, _allocator,
						_connection, _hub, frigg::move(request), event.msgRequest);
				(*closure)();
			}else if(request.req_type() == managarm::fs::CntReqType::READ) {
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
		frigg::SharedPtr<Connection> _connection;
		frigg::SharedPtr<EventHub> _hub;

		uint8_t _buffer[128];
	};
}

namespace general {
	struct GetProfileClosure {
		GetProfileClosure(ServiceAllocator &allocator,
				frigg::SharedPtr<EventHub> hub,
				xuniverse::CntRequest<ServiceAllocator> request,
				uint64_t request_id)
		: _allocator(allocator), _hub(frigg::move(hub)), _req(frigg::move(request)),
				_requestId(request_id), _buffer(allocator) { }

		void operator() () {
			xuniverse::SvrResponse<ServiceAllocator> resp(_allocator);
			resp.set_profile(frigg::String<ServiceAllocator>(_allocator, "minimal"));
			resp.set_error(xuniverse::Errors::SUCCESS);
			resp.SerializeToString(&_buffer);
			serviceSend(rootSendChannel(), _requestId, 0, _buffer.data(), _buffer.size(),
					_hub, CALLBACK_MEMBER(this, &GetProfileClosure::onSend));
		}

	private:
		void onSend(AsyncEvent event) {
			assert(event.error == kErrSuccess);
		}

		ServiceAllocator &_allocator;
		frigg::SharedPtr<EventHub> _hub;
		xuniverse::CntRequest<ServiceAllocator> _req;
		uint64_t _requestId;

		frigg::String<ServiceAllocator> _buffer;
	};

	struct GetServerClosure {
		GetServerClosure(ServiceAllocator &allocator,
				frigg::SharedPtr<EventHub> hub,
				xuniverse::CntRequest<ServiceAllocator> request,
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

				auto connection = frigg::makeShared<initrd::Connection>(_allocator, _allocator,
						frigg::move(local_end));
				auto closure = frigg::construct<initrd::RequestClosure>(_allocator, _allocator,
						frigg::move(connection), _hub);
				(*closure)();

				serviceSendDescriptor(rootSendChannel(), _requestId, 0,
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
		xuniverse::CntRequest<ServiceAllocator> _req;
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

			xuniverse::CntRequest<ServiceAllocator> request(_allocator);
			request.ParseFromArray(_buffer, event.length);
		
			if(request.req_type() == xuniverse::CntReqType::GET_PROFILE) {
				auto closure = frigg::construct<GetProfileClosure>(_allocator, _allocator,
						_hub, frigg::move(request), event.msgRequest);
				(*closure)();
			}else if(request.req_type() == xuniverse::CntReqType::GET_SERVER) {
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
}

void serviceMain() {
	disableInts();
	frigg::infoLogger() << "In service thread" << frigg::endLog;
	
	AllocatorPolicy policy;
	ServiceAllocator allocator(policy);

	frigg::SharedPtr<EventHub> hub = frigg::makeShared<EventHub>(*kernelAlloc);

	auto closure = frigg::construct<general::RequestClosure>(allocator, allocator, hub);
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

