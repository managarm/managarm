
#include "kernel.hpp"
#include "module.hpp"

#include <frg/string.hpp>
#include <frigg/callback.hpp>

#include <posix.frigg_pb.hpp>
#include <fs.frigg_pb.hpp>

#include "fiber.hpp"
#include "service_helpers.hpp"
#include "../arch/x86/debug.hpp"

namespace thor {

namespace posix = managarm::posix;
namespace fs = managarm::fs;

namespace {
	struct ManagarmProcessData {
		HelHandle posixLane;
		HelHandle *fileTable;
		void *clockTrackerPage;
	};

	struct ManagarmServerData {
		HelHandle controlLane;
	};
}

void serviceAccept(LaneHandle handle,
		frigg::CallbackPtr<void(Error, LaneHandle)> callback) {
	submitAccept(handle, callback);
}

void serviceExtractCreds(LaneHandle handle,
		frigg::CallbackPtr<void(Error, frigg::Array<char, 16>)> callback) {
	submitExtractCredentials(handle, callback);
}

void serviceRecv(LaneHandle handle, void *buffer, size_t max_length,
		frigg::CallbackPtr<void(Error, size_t)> callback) {
	submitRecvBuffer(handle,
			KernelAccessor::acquire(buffer, max_length), callback);
}

void serviceRecvInline(LaneHandle handle,
		frigg::CallbackPtr<void(Error, frigg::UniqueMemory<KernelAlloc>)> callback) {
	submitRecvInline(handle, callback);
}

void serviceSend(LaneHandle handle, const void *buffer, size_t length,
		frigg::CallbackPtr<void(Error)> callback) {
	frigg::UniqueMemory<KernelAlloc> kernel_buffer(*kernelAlloc, length);
	memcpy(kernel_buffer.data(), buffer, length);

	submitSendBuffer(handle, frigg::move(kernel_buffer), callback);
}

struct OpenFile {
	OpenFile()
	: isTerminal{false} { };

	bool isTerminal;
	LaneHandle clientLane;
};

struct StdioFile : OpenFile {
	StdioFile() {
		isTerminal = true;
	}
};

namespace stdio {
	struct WriteClosure {
		WriteClosure(LaneHandle lane, fs::CntRequest<KernelAlloc> req)
		: _lane(frigg::move(lane)), _req(frigg::move(req)), _buffer(*kernelAlloc) { }

		void operator() () {
			serviceExtractCreds(_lane, CALLBACK_MEMBER(this, &WriteClosure::onExtractCreds));
		}

	private:
		void onExtractCreds(Error error, frigg::Array<char, 16>) {
			assert(error == kErrSuccess);

			serviceRecvInline(_lane, CALLBACK_MEMBER(this, &WriteClosure::onRecvData));
		}

		void onRecvData(Error error, frigg::UniqueMemory<KernelAlloc> data) {
			assert(error == kErrSuccess);

			{
				auto p = frigg::infoLogger();
				for(size_t i = 0; i < data.size(); i++)
					p.print(reinterpret_cast<char *>(data.data())[i]);
			}

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

	struct SeekClosure {
		SeekClosure(LaneHandle lane, fs::CntRequest<KernelAlloc> req)
		: _lane(frigg::move(lane)), _req(frigg::move(req)), _buffer(*kernelAlloc) { }

		void operator() () {
			fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::fs::Errors::SEEK_ON_PIPE);

			resp.SerializeToString(&_buffer);
			serviceSend(_lane, _buffer.data(), _buffer.size(),
					CALLBACK_MEMBER(this, &SeekClosure::onSendResp));
		}

	private:
		void onSendResp(Error error) {
			assert(error == kErrSuccess);
		}

		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frigg::String<KernelAlloc> _buffer;
	};

	struct RequestClosure {
		RequestClosure(LaneHandle lane)
		: _lane{frigg::move(lane)}, _errorBuffer{*kernelAlloc} { }

		void operator() () {
			serviceAccept(_lane,
					CALLBACK_MEMBER(this, &RequestClosure::onAccept));
		}

	private:
		void onAccept(Error error, LaneHandle handle) {
			assert(error == kErrSuccess);

			_requestLane = frigg::move(handle);
			serviceRecv(_requestLane, _buffer, 128,
					CALLBACK_MEMBER(this, &RequestClosure::onReceive));
		}

		void onReceive(Error error, size_t length) {
			if(error == kErrEndOfLane)
				return;
			assert(error == kErrSuccess);

			fs::CntRequest<KernelAlloc> req(*kernelAlloc);
			req.ParseFromArray(_buffer, length);

			if(req.req_type() == managarm::fs::CntReqType::WRITE) {
				auto closure = frigg::construct<WriteClosure>(*kernelAlloc,
						frigg::move(_requestLane), frigg::move(req));
				(*closure)();
				(*this)();
			}else if(req.req_type() == managarm::fs::CntReqType::SEEK_REL) {
				auto closure = frigg::construct<SeekClosure>(*kernelAlloc,
						frigg::move(_requestLane), frigg::move(req));
				(*closure)();
				(*this)();
			}else{
				frigg::infoLogger() << "\e[31m" "thor: Illegal request type " << req.req_type()
						<< " for kernel provided stdio file" "\e[39m" << frigg::endLog;

				fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::ILLEGAL_REQUEST);

				resp.SerializeToString(&_errorBuffer);
				serviceSend(_requestLane, _errorBuffer.data(), _errorBuffer.size(),
						CALLBACK_MEMBER(this, &RequestClosure::onSendResp));
			}
		}

		void onSendResp(Error error) {
			assert(error == kErrSuccess || error == kErrTransmissionMismatch);
			_requestLane = LaneHandle{};
			(*this)();
		}

		LaneHandle _lane;

		LaneHandle _requestLane;
		uint8_t _buffer[128];
		frigg::String<KernelAlloc> _errorBuffer;
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
			serviceExtractCreds(_lane, CALLBACK_MEMBER(this, &ReadClosure::onExtractCreds));
		}

	private:
		void onExtractCreds(Error error, frigg::Array<char, 16>) {
			assert(error == kErrSuccess);

			assert(_file->offset <= _file->module->getMemory()->getLength());
			_payload.resize(frigg::min(size_t(_req.size()),
					_file->module->getMemory()->getLength() - _file->offset));

			auto complete = [] (CopyFromBundleNode *ctx) {
				auto self = frg::container_of(ctx, &ReadClosure::_copyNode);

				self->_file->offset += self->_payload.size();

				fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SUCCESS);

				resp.SerializeToString(&self->_buffer);
				serviceSend(self->_lane, self->_buffer.data(), self->_buffer.size(),
						CALLBACK_MEMBER(self, &ReadClosure::onSendResp));
			};
			if(copyFromBundle(_file->module->getMemory().get(), _file->offset,
					_payload.data(), _payload.size(), &_copyNode, complete))
				complete(&_copyNode);
		}

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

			submitPushDescriptor(_lane, MemoryViewDescriptor(_file->module->getMemory()),
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
		void onAccept(Error error, LaneHandle handle) {
			assert(error == kErrSuccess);

			_requestLane = frigg::move(handle);
			serviceRecv(_requestLane, _buffer, 128,
					CALLBACK_MEMBER(this, &FileRequestClosure::onReceive));
		}

		void onReceive(Error error, size_t length) {
			if(error == kErrEndOfLane)
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
		Process(frg::string<KernelAlloc> name, frigg::SharedPtr<Thread> thread)
		: _name{std::move(name)}, _thread(frigg::move(thread)), openFiles(*kernelAlloc) {
			fileTableMemory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, 0x1000);
			auto view = frigg::makeShared<MemorySlice>(*kernelAlloc,
					fileTableMemory, 0, 0x1000);

			auto irq_lock = frigg::guard(&irqMutex());
			AddressSpace::Guard space_guard(&_thread->getAddressSpace()->lock);

			auto error = _thread->getAddressSpace()->map(space_guard, frigg::move(view),
					0, 0, 0x1000,
					AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead,
					&clientFileTable);
			assert(!error);
		}

		frg::string_view name() {
			return _name;
		}

		void attachControl(LaneHandle lane) {
			auto irq_lock = frigg::guard(&irqMutex());
			Universe::Guard universe_guard(&_thread->getUniverse()->lock);

			controlHandle = _thread->getUniverse()->attachDescriptor(universe_guard,
					LaneDescriptor{lane});
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

		frg::string<KernelAlloc> _name;
		frigg::SharedPtr<Thread> _thread;

		Handle controlHandle;
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
			if(!module) {
				posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

				resp.SerializeToString(&_buffer);
				serviceSend(_lane, _buffer.data(), _buffer.size(),
						CALLBACK_MEMBER(this, &OpenClosure::onSendResp));
				return;
			}

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

	struct IsTerminalClosure {
		IsTerminalClosure(Process *process, LaneHandle lane, posix::CntRequest<KernelAlloc> req)
		: _process{process}, _lane{frigg::move(lane)},
				_req{frigg::move(req)}, _buffer{*kernelAlloc} { }

		void operator() () {
			assert((size_t)_req.fd() < _process->openFiles.size());
			auto file = _process->openFiles[_req.fd()];

			posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_mode(file->isTerminal ? 1 : 0);

			resp.SerializeToString(&_buffer);
			serviceSend(_lane, _buffer.data(), _buffer.size(),
					CALLBACK_MEMBER(this, &IsTerminalClosure::onSendResp));
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

	struct ServerRequestClosure {
		ServerRequestClosure(Process *process, LaneHandle lane)
		: _process(process), _lane(frigg::move(lane)) { }

		void operator() () {
			serviceAccept(_lane,
					CALLBACK_MEMBER(this, &ServerRequestClosure::onAccept));
		}

	private:
		void onAccept(Error error, LaneHandle handle) {
			assert(error == kErrSuccess);

			_requestLane = frigg::move(handle);
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
			}else if(req.request_type() == managarm::posix::CntReqType::IS_TTY) {
				auto closure = frigg::construct<IsTerminalClosure>(*kernelAlloc, _process,
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

		Process *_process;
		LaneHandle _lane;

		LaneHandle _requestLane;
		uint8_t _buffer[128];
	};

	struct ObserveClosure {
		ObserveClosure(Process *process, frigg::SharedPtr<Thread> thread)
		: _process(process), _thread(frigg::move(thread)),
				_observedSeq{1} { }

		void operator() () {
			_thread->submitObserve(_observedSeq,
					CALLBACK_MEMBER(this, &ObserveClosure::onObserve));
		}

	private:
		void onObserve(Error error, uint64_t sequence, Interrupt interrupt) {
			assert(error == kErrSuccess);
			_observedSeq = sequence;

			if(interrupt == kIntrPanic) {
				// Do nothing and stop observing.
				// TODO: Make sure the server is destructed here.
				frigg::infoLogger() << "\e[31m" "thor: Panic in server "
						<< _process->name().data() << "\e[39m" << frigg::endLog;
				return;
			}else if(interrupt == kIntrPageFault) {
				// Do nothing and stop observing.
				// TODO: Make sure the server is destructed here.
				frigg::infoLogger() << "\e[31m" "thor: Fault in server "
						<< _process->name().data() << "\e[39m" << frigg::endLog;
				return;
			}else if(interrupt == kIntrSuperCall + 1) {
				AcquireNode node;

				_spaceLock = AddressSpaceLockHandle{_thread->getAddressSpace().lock(),
						reinterpret_cast<void *>(_thread->_executor.general()->rsi),
						sizeof(ManagarmProcessData)};
				_worklet.setup(&ObserveClosure::onProcessDataAcquire);
				_acquire.setup(&_worklet);
				auto acq = _spaceLock.acquire(&_acquire);
				if(acq)
					WorkQueue::post(&_worklet);
			}else if(interrupt == kIntrSuperCall + 64) {
				AcquireNode node;

				_spaceLock = AddressSpaceLockHandle{_thread->getAddressSpace().lock(),
						reinterpret_cast<void *>(_thread->_executor.general()->rsi),
						sizeof(ManagarmServerData)};
				_worklet.setup(&ObserveClosure::onServerDataAcquire);
				_acquire.setup(&_worklet);
				auto acq = _spaceLock.acquire(&_acquire);
				if(acq)
					WorkQueue::post(&_worklet);
			}else if(interrupt == kIntrSuperCall + 7) { // sigprocmask.
				_thread->_executor.general()->rdi = kHelErrNone;
				_thread->_executor.general()->rsi = 0;
				if(auto e = Thread::resumeOther(_thread); e)
					frigg::panicLogger() << "thor: Failed to resume server" << frigg::endLog;

				(*this)();
			}else{
				frigg::panicLogger() << "thor: Unexpected observation "
						<< (uint32_t)interrupt << frigg::endLog;
			}
		}

		static void onProcessDataAcquire(Worklet *base) {
			auto self = frg::container_of(base, &ObserveClosure::_worklet);

			ManagarmProcessData data = {
				kHelThisThread,
				reinterpret_cast<HelHandle *>(self->_process->clientFileTable),
				nullptr
			};

			self->_spaceLock.write(0, &data, sizeof(ManagarmProcessData));
			self->_spaceLock = {};

			self->_thread->_executor.general()->rdi = kHelErrNone;
			if(auto e = Thread::resumeOther(self->_thread); e)
				frigg::panicLogger() << "thor: Failed to resume server" << frigg::endLog;

			(*self)();
		}

		static void onServerDataAcquire(Worklet *base) {
			auto self = frg::container_of(base, &ObserveClosure::_worklet);

			ManagarmServerData data = {
				self->_process->controlHandle
			};

			self->_spaceLock.write(0, &data, sizeof(ManagarmServerData));
			self->_spaceLock = {};

			self->_thread->_executor.general()->rdi = kHelErrNone;
			if(auto e = Thread::resumeOther(self->_thread); e)
				frigg::panicLogger() << "thor: Failed to resume server" << frigg::endLog;

			(*self)();
		}

		Process *_process;
		frigg::SharedPtr<Thread> _thread;

		uint64_t _observedSeq;
		Worklet _worklet;
		AddressSpaceLockHandle _spaceLock;
		AcquireNode _acquire;
	};
}

void runService(frg::string<KernelAlloc> name, LaneHandle control_lane,
		frigg::SharedPtr<Thread> thread) {
	KernelFiber::run([name, thread, control_lane = std::move(control_lane)] () mutable {
		auto stdio_stream = createStream();
		auto stdio_file = frigg::construct<StdioFile>(*kernelAlloc);
		stdio_file->clientLane = frigg::move(stdio_stream.get<1>());

		auto stdio_closure = frigg::construct<stdio::RequestClosure>(*kernelAlloc,
				frigg::move(stdio_stream.get<0>()));
		(*stdio_closure)();

		auto process = frigg::construct<initrd::Process>(*kernelAlloc, std::move(name), thread);
		process->attachControl(std::move(control_lane));
		process->attachFile(stdio_file);
		process->attachFile(stdio_file);
		process->attachFile(stdio_file);

		auto observe_closure = frigg::construct<initrd::ObserveClosure>(*kernelAlloc,
				process, thread);
		(*observe_closure)();

		auto posix_closure = frigg::construct<initrd::ServerRequestClosure>(*kernelAlloc,
				process, thread->superiorLane());
		(*posix_closure)();

		// Just block this fiber forever (we're still processing worklets).
		FiberBlocker blocker;
		blocker.setup();
		KernelFiber::blockCurrent(&blocker);
	});
}

} // namespace thor

