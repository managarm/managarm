
#include "kernel.hpp"
#include "module.hpp"

#include <frigg/callback.hpp>

#include <posix.frigg_pb.hpp>
#include <fs.frigg_pb.hpp>

#include "../arch/x86/debug.hpp"

namespace thor {

// TODO: move this to a header file
extern frigg::LazyInitializer<frigg::SharedPtr<Endpoint, EndpointRwControl>> initrdServer;

namespace posix = managarm::posix;
namespace fs = managarm::fs;

template<typename S>
struct UseCallback;

template<typename... Args>
struct UseCallback<void(Args...)> {
	struct Completer {
		explicit Completer(UseCallback token)
		: _callback(token._callback) { }

		void operator() (Args &&... args) {
			_callback(frigg::forward<Args>(args)...);
		}

	private:
		frigg::CallbackPtr<void(Args...)> _callback;
	};

	explicit UseCallback(frigg::CallbackPtr<void(Args...)> callback)
	: _callback(callback) { }

private:
	frigg::CallbackPtr<void(Args...)> _callback;
};

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

void serviceAccept(LaneHandle handle,
		frigg::CallbackPtr<void(Error, frigg::WeakPtr<Universe>, LaneDescriptor)> callback) {
	handle.getStream()->submitAccept(handle.getLane(), frigg::WeakPtr<Universe>(),
			UseCallback<void(Error, frigg::WeakPtr<Universe>, LaneDescriptor)>(callback));
}

void serviceRecv(LaneHandle handle, void *buffer, size_t max_length,
		frigg::CallbackPtr<void(Error, size_t)> callback) {
	handle.getStream()->submitRecvBuffer(handle.getLane(),
			KernelAccessor::acquire(buffer, max_length),
			UseCallback<void(Error, size_t)>(callback));
}

void serviceSend(LaneHandle handle, const void *buffer, size_t length,
		frigg::CallbackPtr<void(Error)> callback) {
	frigg::UniqueMemory<KernelAlloc> kernel_buffer(*kernelAlloc, length);
	memcpy(kernel_buffer.data(), buffer, length);
	
	handle.getStream()->submitSendBuffer(handle.getLane(), frigg::move(kernel_buffer),
			UseCallback<void(Error)>(callback));
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
		FileConnection(ServiceAllocator &, OpenFile *file, LaneHandle lane)
		: file(file), lane(frigg::move(lane)) { }

		OpenFile *file;
		LaneHandle lane;
	};
	
	struct SeekClosure {
		SeekClosure(OpenFile *file, LaneHandle lane, fs::CntRequest<KernelAlloc> req)
		: _file(file), _lane(frigg::move(lane)), _req(frigg::move(req)),
				_buffer(*kernelAlloc) { }

		void operator() () {
			_file->offset = _req.rel_offset();

			fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			resp.SerializeToString(&_buffer);
			serviceSend(_lane, _buffer.data(), _buffer.size(),
					CALLBACK_MEMBER(this, &SeekClosure::onSend));
		}

	private:
		void onSend(Error error) {
			assert(error == kErrSuccess);
		}

		OpenFile *_file;
		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frigg::String<KernelAlloc> _buffer;
	};
	
	struct ReadClosure {
		ReadClosure(OpenFile *file, LaneHandle lane, fs::CntRequest<KernelAlloc> req)
		: _file(file), _lane(frigg::move(lane)), _req(frigg::move(req)),
				_buffer(*kernelAlloc), _payload(*kernelAlloc) { }

		void operator() () {
			assert(_file->offset <= _file->module->length);
			_payload.resize(frigg::min(size_t(_req.size()),
					_file->module->length - _file->offset));
			assert(_payload.size());
			void *src = physicalToVirtual(_file->module->physical
					+ _file->offset);
			memcpy(_payload.data(), src, _payload.size());
			_file->offset += _payload.size();

			fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			resp.SerializeToString(&_buffer);
			serviceSend(_lane, _buffer.data(), _buffer.size(),
					CALLBACK_MEMBER(this, &ReadClosure::onSendResp));
		}

	private:
		void onSendResp(Error error) {
			assert(error == kErrSuccess);
			
			serviceSend(_lane, _payload.data(), _payload.size(),
					CALLBACK_MEMBER(this, &ReadClosure::onSendData));
		}
		
		void onSendData(Error error) {
			assert(error == kErrSuccess);
		}

		OpenFile *_file;
		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frigg::String<KernelAlloc> _buffer;
		frigg::String<KernelAlloc> _payload;
	};
	
	struct MapClosure {
		MapClosure(OpenFile *file, LaneHandle lane, fs::CntRequest<KernelAlloc> req)
		: _file(file), _lane(frigg::move(lane)), _req(frigg::move(req)),
				_buffer(*kernelAlloc) { }

		void operator() () {
			fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			resp.SerializeToString(&_buffer);
			serviceSend(_lane, _buffer.data(), _buffer.size(),
					CALLBACK_MEMBER(this, &MapClosure::onSendResp));
		}

	private:
		void onSendResp(Error error) {
			assert(error == kErrSuccess);
			
			size_t virt_length = _file->module->length;
			if(virt_length % kPageSize)
				virt_length += kPageSize - (virt_length % kPageSize);

			auto memory = frigg::makeShared<Memory>(*kernelAlloc,
					HardwareMemory(_file->module->physical, virt_length));
			_lane.getStream()->submitPushDescriptor(_lane.getLane(),
					MemoryAccessDescriptor(frigg::move(memory)),
					UseCallback<void(Error)>(CALLBACK_MEMBER(this, &MapClosure::onSendHandle)));
		}
		
		void onSendHandle(Error error) {
			assert(error == kErrSuccess);
		}

		OpenFile *_file;
		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frigg::String<KernelAlloc> _buffer;
	};

	struct FileRequestClosure {
		FileRequestClosure(LaneHandle lane, OpenFile *file)
		: _lane(frigg::move(lane)), _file(file) { }

		void operator() () {
			serviceAccept(_lane,
					CALLBACK_MEMBER(this, &FileRequestClosure::onAccept));
		}

	private:
		void onAccept(Error error, frigg::WeakPtr<Universe>, LaneDescriptor descriptor) {
			assert(error == kErrSuccess);

			_requestLane = frigg::move(descriptor.handle);
			serviceRecv(_requestLane, _buffer, 128,
					CALLBACK_MEMBER(this, &FileRequestClosure::onReceive));
		}

		void onReceive(Error error, size_t length) {
			if(error == kErrClosedRemotely)
				return;
			assert(error == kErrSuccess);

			fs::CntRequest<KernelAlloc> req(*kernelAlloc);
			req.ParseFromArray(_buffer, length);

/*			if(req.req_type() == managarm::fs::CntReqType::FSTAT) {
				auto closure = frigg::construct<StatClosure>(*allocator,
						*this, msg_request, frigg::move(req));
				(*closure)();
			}else*/ if(req.req_type() == managarm::fs::CntReqType::READ) {
				auto closure = frigg::construct<ReadClosure>(*kernelAlloc,
						_file, frigg::move(_requestLane), frigg::move(req));
				(*closure)();
			}else if(req.req_type() == managarm::fs::CntReqType::SEEK_ABS) {
				auto closure = frigg::construct<SeekClosure>(*kernelAlloc,
						_file, frigg::move(_requestLane), frigg::move(req));
				(*closure)();
			}else if(req.req_type() == managarm::fs::CntReqType::MMAP) {
				auto closure = frigg::construct<MapClosure>(*kernelAlloc,
						_file, frigg::move(_requestLane), frigg::move(req));
				(*closure)();
			}else{
				frigg::panicLogger() << "Illegal request type" << frigg::endLog;
			}

			(*this)();
		}

		LaneHandle _lane;
		OpenFile *_file;

		LaneHandle _requestLane;

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

			auto lanes = createStream();

			auto file = frigg::construct<OpenFile>(*kernelAlloc, module);
			auto closure = frigg::construct<initrd::FileRequestClosure>(*kernelAlloc,
					frigg::move(lanes.get<0>()), file);
			(*closure)();
			
			serviceSendDescriptor(Endpoint::writeChannel(_connection->endpoint), _requestId, 1,
					LaneDescriptor(frigg::move(lanes.get<1>())),
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

