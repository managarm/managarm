
#include "kernel.hpp"
#include "module.hpp"

#include <frigg/callback.hpp>

#include <posix.frigg_pb.hpp>
#include <fs.frigg_pb.hpp>

#include "../arch/x86/debug.hpp"

namespace thor {

namespace posix = managarm::posix;
namespace fs = managarm::fs;

void serviceAccept(LaneHandle handle,
		frigg::CallbackPtr<void(Error, frigg::WeakPtr<Universe>, LaneDescriptor)> callback) {
	handle.getStream()->submitAccept(handle.getLane(), frigg::WeakPtr<Universe>(), callback);
}

void serviceRecv(LaneHandle handle, void *buffer, size_t max_length,
		frigg::CallbackPtr<void(Error, size_t)> callback) {
	handle.getStream()->submitRecvBuffer(handle.getLane(),
			KernelAccessor::acquire(buffer, max_length), callback);
}

void serviceSend(LaneHandle handle, const void *buffer, size_t length,
		frigg::CallbackPtr<void(Error)> callback) {
	frigg::UniqueMemory<KernelAlloc> kernel_buffer(*kernelAlloc, length);
	memcpy(kernel_buffer.data(), buffer, length);
	
	handle.getStream()->submitSendBuffer(handle.getLane(), frigg::move(kernel_buffer), callback);
}

namespace stdio {
	struct WriteClosure {
		WriteClosure(LaneHandle lane, fs::CntRequest<KernelAlloc> req)
		: _lane(frigg::move(lane)), _req(frigg::move(req)), _buffer(*kernelAlloc) { }

		void operator() () {
			serviceRecv(_lane, _data, 128,
					CALLBACK_MEMBER(this, &WriteClosure::onRecvData));
		}

	private:
		void onRecvData(Error error, size_t length) {
			assert(error == kErrSuccess);
			
			helLog(_data, length);

			fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::fs::Errors::SUCCESS);

			resp.SerializeToString(&_buffer);
			serviceSend(_lane, _buffer.data(), _buffer.size(),
					CALLBACK_MEMBER(this, &WriteClosure::onSendResp));
		}
		
		void onSendResp(Error error) {
			assert(error == kErrSuccess);
		}

		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		char _data[128];
		frigg::String<KernelAlloc> _buffer;
	};

	struct RequestClosure {
		RequestClosure(LaneHandle lane)
		: _lane(frigg::move(lane)) { }

		void operator() () {
			serviceAccept(_lane,
					CALLBACK_MEMBER(this, &RequestClosure::onAccept));
		}

	private:
		void onAccept(Error error, frigg::WeakPtr<Universe>, LaneDescriptor descriptor) {
			assert(error == kErrSuccess);

			_requestLane = frigg::move(descriptor.handle);
			serviceRecv(_requestLane, _buffer, 128,
					CALLBACK_MEMBER(this, &RequestClosure::onReceive));
		}

		void onReceive(Error error, size_t length) {
			if(error == kErrClosedRemotely)
				return;
			assert(error == kErrSuccess);

			fs::CntRequest<KernelAlloc> req(*kernelAlloc);
			req.ParseFromArray(_buffer, length);

			if(req.req_type() == managarm::fs::CntReqType::WRITE) {
				auto closure = frigg::construct<WriteClosure>(*kernelAlloc,
						frigg::move(_requestLane), frigg::move(req));
				(*closure)();
			}else{
				frigg::panicLogger() << "Illegal request type " << req.req_type()
						<< " kernel provided stdio file" << frigg::endLog;
			}

			(*this)();
		}

		LaneHandle _lane;

		LaneHandle _requestLane;
		uint8_t _buffer[128];
	};
} // namespace stdio

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

	struct SeekClosure {
		SeekClosure(OpenFile *file, LaneHandle lane, fs::CntRequest<KernelAlloc> req)
		: _file(file), _lane(frigg::move(lane)), _req(frigg::move(req)),
				_buffer(*kernelAlloc) { }

		void operator() () {
			_file->offset = _req.rel_offset();

			fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_offset(_file->offset);

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
					CALLBACK_MEMBER(this, &MapClosure::onSendHandle));
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
				frigg::panicLogger() << "Illegal request type " << req.req_type()
						<< " for kernel provided initrd file" << frigg::endLog;
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

	struct OpenClosure {
		OpenClosure(LaneHandle lane, posix::CntRequest<KernelAlloc> req)
		: _lane(frigg::move(lane)), _req(frigg::move(req)), _buffer(*kernelAlloc) { }

		void operator() () {
			frigg::infoLogger() << "initrd: '" <<  _req.path() << "' requested." << frigg::endLog;
			posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(0); // TODO: for now we can get away without handling fds.

			resp.SerializeToString(&_buffer);
			serviceSend(_lane, _buffer.data(), _buffer.size(),
					CALLBACK_MEMBER(this, &OpenClosure::onSendResp));
		}

	private:
		void onSendResp(Error error) {
			assert(error == kErrSuccess);
			
			// TODO: this should not be handled here!
			Module *module = getModule(_req.path());
			assert(module);

			auto lanes = createStream();

			auto file = frigg::construct<OpenFile>(*kernelAlloc, module);
			auto closure = frigg::construct<initrd::FileRequestClosure>(*kernelAlloc,
					frigg::move(lanes.get<0>()), file);
			(*closure)();
			
			_lane.getStream()->submitPushDescriptor(_lane.getLane(),
					LaneDescriptor(frigg::move(lanes.get<1>())),
					CALLBACK_MEMBER(this, &OpenClosure::onSendHandle));
		}

		void onSendHandle(Error error) {
			assert(error == kErrSuccess);
		}

		LaneHandle _lane;
		posix::CntRequest<KernelAlloc> _req;

		frigg::String<KernelAlloc> _buffer;
	};
	
	struct CloseClosure {
		CloseClosure(LaneHandle lane, posix::CntRequest<KernelAlloc> req)
		: _lane(frigg::move(lane)), _req(frigg::move(req)), _buffer(*kernelAlloc) { }

		void operator() () {
			// TODO: for now we just ignore close requests.
			posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::posix::Errors::SUCCESS);

			resp.SerializeToString(&_buffer);
			serviceSend(_lane, _buffer.data(), _buffer.size(),
					CALLBACK_MEMBER(this, &CloseClosure::onSendResp));
		}

	private:
		void onSendResp(Error error) {
			assert(error == kErrSuccess);
		}

		LaneHandle _lane;
		posix::CntRequest<KernelAlloc> _req;

		frigg::String<KernelAlloc> _buffer;
	};

	struct ServerRequestClosure {
		ServerRequestClosure(LaneHandle lane)
		: _lane(frigg::move(lane)) { }

		void operator() () {
			serviceAccept(_lane,
					CALLBACK_MEMBER(this, &ServerRequestClosure::onAccept));
		}

	private:
		void onAccept(Error error, frigg::WeakPtr<Universe>, LaneDescriptor descriptor) {
			assert(error == kErrSuccess);

			_requestLane = frigg::move(descriptor.handle);
			serviceRecv(_requestLane, _buffer, 128,
					CALLBACK_MEMBER(this, &ServerRequestClosure::onReceive));
		}
		
		void onReceive(Error error, size_t length) {
			assert(error == kErrSuccess);

			posix::CntRequest<KernelAlloc> req(*kernelAlloc);
			req.ParseFromArray(_buffer, length);

			if(req.request_type() == managarm::posix::CntReqType::OPEN) {
				auto closure = frigg::construct<OpenClosure>(*kernelAlloc,
						frigg::move(_requestLane), frigg::move(req));
				(*closure)();
			}else if(req.request_type() == managarm::posix::CntReqType::CLOSE) {
				auto closure = frigg::construct<CloseClosure>(*kernelAlloc,
						frigg::move(_requestLane), frigg::move(req));
				(*closure)();
			}else{
				frigg::panicLogger() << "Illegal POSIX request type "
						<< req.request_type() << frigg::endLog;
			}

			(*this)();
		}

		LaneHandle _lane;

		LaneHandle _requestLane;
		uint8_t _buffer[128];
	};
}

void runStdio(LaneHandle lane) {
	auto closure = frigg::construct<stdio::RequestClosure>(*kernelAlloc,
			frigg::move(lane));
	(*closure)();
}

void runService(LaneHandle lane) {
	// start the initrd server.
	auto closure = frigg::construct<initrd::ServerRequestClosure>(*kernelAlloc,
			frigg::move(lane));
	(*closure)();
}

} // namespace thor

