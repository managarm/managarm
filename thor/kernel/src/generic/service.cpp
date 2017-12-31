
#include "kernel.hpp"
#include "module.hpp"

#include <frigg/callback.hpp>

#include <posix.frigg_pb.hpp>
#include <fs.frigg_pb.hpp>

#include "fiber.hpp"
#include "service_helpers.hpp"
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

void serviceRecvInline(LaneHandle handle,
		frigg::CallbackPtr<void(Error, frigg::UniqueMemory<KernelAlloc>)> callback) {
	handle.getStream()->submitRecvInline(handle.getLane(), callback);
}

void serviceSend(LaneHandle handle, const void *buffer, size_t length,
		frigg::CallbackPtr<void(Error)> callback) {
	frigg::UniqueMemory<KernelAlloc> kernel_buffer(*kernelAlloc, length);
	memcpy(kernel_buffer.data(), buffer, length);
	
	handle.getStream()->submitSendBuffer(handle.getLane(), frigg::move(kernel_buffer), callback);
}

struct OpenFile {
	LaneHandle clientLane;
};

struct StdioFile : OpenFile {
};

namespace stdio {
	struct WriteClosure {
		WriteClosure(LaneHandle lane, fs::CntRequest<KernelAlloc> req)
		: _lane(frigg::move(lane)), _req(frigg::move(req)), _buffer(*kernelAlloc) { }

		void operator() () {
			serviceRecvInline(_lane, CALLBACK_MEMBER(this, &WriteClosure::onRecvData));
		}

	private:
		void onRecvData(Error error, frigg::UniqueMemory<KernelAlloc> data) {
			assert(error == kErrSuccess);
			
			helLog(reinterpret_cast<char *>(data.data()), data.size());

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
						<< " for kernel provided stdio file" << frigg::endLog;
			}

			(*this)();
		}

		LaneHandle _lane;

		LaneHandle _requestLane;
		uint8_t _buffer[128];
	};
} // namespace stdio

namespace initrd {
	struct ModuleFile : OpenFile {
		ModuleFile(MfsRegular *module)
		: module(module), offset(0) { }

		MfsRegular *module;
		size_t offset;
	};

	// ----------------------------------------------------
	// initrd file handling.
	// ----------------------------------------------------

	struct SeekClosure {
		SeekClosure(ModuleFile *file, LaneHandle lane, fs::CntRequest<KernelAlloc> req)
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

		ModuleFile *_file;
		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frigg::String<KernelAlloc> _buffer;
	};
	
	struct ReadClosure {
		ReadClosure(ModuleFile *file, LaneHandle lane, fs::CntRequest<KernelAlloc> req)
		: _file(file), _lane(frigg::move(lane)), _req(frigg::move(req)),
				_buffer(*kernelAlloc), _payload(*kernelAlloc) { }

		void operator() () {
			assert(_file->offset <= _file->module->getMemory()->getLength());
			_payload.resize(frigg::min(size_t(_req.size()),
					_file->module->getMemory()->getLength() - _file->offset));
			copyFromBundle(_file->module->getMemory().get(), _file->offset,
					_payload.data(), _payload.size(), &_copyNode, [] (CopyFromBundleNode *ctx) {
				auto self = frg::container_of(ctx, &ReadClosure::_copyNode);

				self->_file->offset += self->_payload.size();

				fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SUCCESS);

				resp.SerializeToString(&self->_buffer);
				serviceSend(self->_lane, self->_buffer.data(), self->_buffer.size(),
						CALLBACK_MEMBER(self, &ReadClosure::onSendResp));
			});
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

		ModuleFile *_file;
		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frigg::String<KernelAlloc> _buffer;
		frigg::String<KernelAlloc> _payload;
		CopyFromBundleNode _copyNode;
	};
	
	struct MapClosure {
		MapClosure(ModuleFile *file, LaneHandle lane, fs::CntRequest<KernelAlloc> req)
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

			_lane.getStream()->submitPushDescriptor(_lane.getLane(),
					MemoryAccessDescriptor(_file->module->getMemory()),
					CALLBACK_MEMBER(this, &MapClosure::onSendHandle));
		}
		
		void onSendHandle(Error error) {
			assert(error == kErrSuccess);
		}

		ModuleFile *_file;
		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frigg::String<KernelAlloc> _buffer;
	};

	struct FileRequestClosure {
		FileRequestClosure(LaneHandle lane, ModuleFile *file)
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
		ModuleFile *_file;

		LaneHandle _requestLane;
		uint8_t _buffer[128];
	};
	
	struct OpenDirectory : OpenFile {
		OpenDirectory(MfsDirectory *node)
		: node{node}, index(0) { }

		MfsDirectory *node;
		size_t index;
	};
	
	bool handleDirectoryReq(LaneHandle lane, OpenDirectory *file) {
		auto branch = fiberAccept(lane);
		if(!branch)
			return false;

		auto buffer = fiberRecv(branch);
		managarm::fs::CntRequest<KernelAlloc> req(*kernelAlloc);
		req.ParseFromArray(buffer.data(), buffer.size());
	
		if(req.req_type() == managarm::fs::CntReqType::PT_READ_ENTRIES) {
			if(file->index < file->node->numEntries()) {
				auto entry = file->node->getEntry(file->index);

				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_path(entry.name);
				if(entry.node->type == MfsType::directory) {
					resp.set_file_type(managarm::fs::FileType::DIRECTORY);
				}else{
					assert(entry.node->type == MfsType::regular);
					resp.set_file_type(managarm::fs::FileType::REGULAR);
				}
				
				file->index++;

				frigg::String<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				fiberSend(branch, ser.data(), ser.size());
			}else{
				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::END_OF_FILE);

				frigg::String<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				fiberSend(branch, ser.data(), ser.size());
			}
		}else{
			managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::fs::Errors::ILLEGAL_REQUEST);
			
			frigg::String<KernelAlloc> ser(*kernelAlloc);
			resp.SerializeToString(&ser);
			fiberSend(branch, ser.data(), ser.size());
		}

		return true;
	}

	// ----------------------------------------------------
	// POSIX server.
	// ----------------------------------------------------

	struct Process {
		Process(frigg::SharedPtr<Thread> thread)
		: _thread(frigg::move(thread)), openFiles(*kernelAlloc) {
			fileTableMemory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, 0x1000);

			auto irq_lock = frigg::guard(&irqMutex());
			AddressSpace::Guard space_guard(&_thread->getAddressSpace()->lock);

			_thread->getAddressSpace()->map(space_guard, fileTableMemory, 0, 0, 0x1000,
					AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead,
					&clientFileTable);
		}

		int attachFile(OpenFile *file) {
			Handle handle;
			{
				auto irq_lock = frigg::guard(&irqMutex());
				Universe::Guard universe_guard(&_thread->getUniverse()->lock);

				handle = _thread->getUniverse()->attachDescriptor(universe_guard,
						LaneDescriptor(file->clientLane));
			}

			// TODO: Get rid of copyKernelToThisSync()?

			for(int fd = 0; fd < (int)openFiles.size(); ++fd) {
				if(openFiles[fd])
					continue;
				openFiles[fd] = file;
				fileTableMemory->copyKernelToThisSync(sizeof(Handle) * fd,
						&handle, sizeof(Handle));
				return fd;
			}

			int fd = openFiles.size();
			openFiles.push(file);
			fileTableMemory->copyKernelToThisSync(sizeof(Handle) * fd,
					&handle, sizeof(Handle));
			return fd;
		}

		frigg::SharedPtr<Thread> _thread;

		frigg::Vector<OpenFile *, KernelAlloc> openFiles;

		frigg::SharedPtr<Memory> fileTableMemory;

		VirtualAddr clientFileTable;
	};

	struct OpenClosure {
		OpenClosure(Process *process, LaneHandle lane, posix::CntRequest<KernelAlloc> req)
		: _process(process), _lane(frigg::move(lane)), _req(frigg::move(req)),
				_buffer(*kernelAlloc) { }

		void operator() () {
//			frigg::infoLogger() << "initrd: '" <<  _req.path() << "' requested." << frigg::endLog;
			// TODO: Actually handle the file-not-found case.
			auto module = resolveModule(_req.path());
			if(!module)
				frigg::panicLogger() << "initrd: Module '" << _req.path()
						<< "' not found" << frigg::endLog;

			if(module->type == MfsType::directory) {
				auto stream = createStream();
				auto file = frigg::construct<OpenDirectory>(*kernelAlloc,
						static_cast<MfsDirectory *>(module));
				file->clientLane = frigg::move(stream.get<1>());

				KernelFiber::run([lane = stream.get<0>(), file] () {
					while(true) {
						if(!handleDirectoryReq(lane, file))
							break;
					}
				});
				
				auto fd = _process->attachFile(file);

				posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd);

				resp.SerializeToString(&_buffer);
				serviceSend(_lane, _buffer.data(), _buffer.size(),
						CALLBACK_MEMBER(this, &OpenClosure::onSendResp));
			}else{
				assert(module->type == MfsType::regular);
				
				auto stream = createStream();
				auto file = frigg::construct<ModuleFile>(*kernelAlloc,
						static_cast<MfsRegular *>(module));
				file->clientLane = frigg::move(stream.get<1>());

				auto closure = frigg::construct<initrd::FileRequestClosure>(*kernelAlloc,
						frigg::move(stream.get<0>()), file);
				(*closure)();

				auto fd = _process->attachFile(file);

				posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd);

				resp.SerializeToString(&_buffer);
				serviceSend(_lane, _buffer.data(), _buffer.size(),
						CALLBACK_MEMBER(this, &OpenClosure::onSendResp));
			}
		}

	private:
		void onSendResp(Error error) {
			assert(error == kErrSuccess);
		}

		Process *_process;
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
		ServerRequestClosure(Process *process, LaneHandle lane)
		: _process(process), _lane(frigg::move(lane)) { }

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
						_process, frigg::move(_requestLane), frigg::move(req));
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

		Process *_process;
		LaneHandle _lane;

		LaneHandle _requestLane;
		uint8_t _buffer[128];
	};
	
	struct ObserveClosure {
		ObserveClosure(Process *process, frigg::SharedPtr<Thread> thread)
		: _process(process), _thread(frigg::move(thread)) { }

		void operator() () {
			_thread->submitObserve(CALLBACK_MEMBER(this, &ObserveClosure::onObserve));
		}

	private:
		void onObserve(Error error, Interrupt interrupt) {
			assert(error == kErrSuccess);
			
			if(interrupt == kIntrSuperCall + 1) {
				_thread->_executor.general()->rdi = kHelErrNone;
				_thread->_executor.general()->rsi = (Word)_process->clientFileTable;
				Thread::resumeOther(_thread);
			}else{
				frigg::panicLogger() << "Unexpected observation" << frigg::endLog;
			}

			(*this)();
		}
		
		Process *_process;
		frigg::SharedPtr<Thread> _thread;
	};
}

void runService(frigg::SharedPtr<Thread> thread) {
	auto stdio_stream = createStream();
	auto stdio_file = frigg::construct<StdioFile>(*kernelAlloc);
	stdio_file->clientLane = frigg::move(stdio_stream.get<1>());
	
	auto stdio_closure = frigg::construct<stdio::RequestClosure>(*kernelAlloc,
			frigg::move(stdio_stream.get<0>()));
	(*stdio_closure)();

	auto process = frigg::construct<initrd::Process>(*kernelAlloc, thread);
	process->attachFile(stdio_file);
	process->attachFile(stdio_file);
	process->attachFile(stdio_file);
	
	auto observe_closure = frigg::construct<initrd::ObserveClosure>(*kernelAlloc,
			process, thread);
	(*observe_closure)();

	auto posix_closure = frigg::construct<initrd::ServerRequestClosure>(*kernelAlloc,
			process, thread->superiorLane());
	(*posix_closure)();
}

} // namespace thor

