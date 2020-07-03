
#include "kernel.hpp"
#include "module.hpp"

#include <bragi/helpers-frigg.hpp>
#include <frg/span.hpp>
#include <frg/string.hpp>
#include <frigg/callback.hpp>

#include <posix.frigg_bragi.hpp>
#include <fs.frigg_pb.hpp>

#include "execution/coroutine.hpp"
#include "fiber.hpp"
#include "service_helpers.hpp"
#include "../arch/x86/debug.hpp"

namespace thor {

namespace posix = managarm::posix;
namespace fs = managarm::fs;

namespace {
	struct ManagarmProcessData {
		HelHandle posixLane;
		void *threadPage;
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
			assert(error == Error::success);

			serviceRecvInline(_lane, CALLBACK_MEMBER(this, &WriteClosure::onRecvData));
		}

		void onRecvData(Error error, frigg::UniqueMemory<KernelAlloc> data) {
			assert(error == Error::success);

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
			assert(error == Error::success);
			frigg::destruct(*kernelAlloc, this);
		}

		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frg::string<KernelAlloc> _buffer;
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
			assert(error == Error::success);
		}

		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frg::string<KernelAlloc> _buffer;
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
			assert(error == Error::success);

			_requestLane = frigg::move(handle);
			serviceRecv(_requestLane, _buffer, 128,
					CALLBACK_MEMBER(this, &RequestClosure::onReceive));
		}

		void onReceive(Error error, size_t length) {
			if(error == Error::endOfLane)
				return;
			assert(error == Error::success);

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
			assert(error == Error::success || error == Error::transmissionMismatch);
			_requestLane = LaneHandle{};
			(*this)();
		}

		LaneHandle _lane;

		LaneHandle _requestLane;
		uint8_t _buffer[128];
		frg::string<KernelAlloc> _errorBuffer;
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
			assert(error == Error::success);
		}

		ModuleFile *_file;
		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frg::string<KernelAlloc> _buffer;
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
			assert(error == Error::success);

			assert(_file->offset <= _file->module->size());
			_payload.resize(frigg::min(size_t(_req.size()),
					_file->module->size() - _file->offset));

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
			assert(error == Error::success);

			serviceSend(_lane, _payload.data(), _payload.size(),
					CALLBACK_MEMBER(this, &ReadClosure::onSendData));
		}

		void onSendData(Error error) {
			assert(error == Error::success);
		}

		ModuleFile *_file;
		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frg::string<KernelAlloc> _buffer;
		frg::string<KernelAlloc> _payload;
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
			assert(error == Error::success);

			submitPushDescriptor(_lane, MemoryViewDescriptor(_file->module->getMemory()),
					CALLBACK_MEMBER(this, &MapClosure::onSendHandle));
		}

		void onSendHandle(Error error) {
			assert(error == Error::success);
		}

		ModuleFile *_file;
		LaneHandle _lane;
		fs::CntRequest<KernelAlloc> _req;

		frg::string<KernelAlloc> _buffer;
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
			assert(error == Error::success);

			_requestLane = frigg::move(handle);
			serviceRecv(_requestLane, _buffer, 128,
					CALLBACK_MEMBER(this, &FileRequestClosure::onReceive));
		}

		void onReceive(Error error, size_t length) {
			if(error == Error::endOfLane)
				return;
			assert(error == Error::success);

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

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				fiberSend(branch, ser.data(), ser.size());
			}else{
				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::END_OF_FILE);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				fiberSend(branch, ser.data(), ser.size());
			}
		}else{
			managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::fs::Errors::ILLEGAL_REQUEST);

			frg::string<KernelAlloc> ser(*kernelAlloc);
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

			auto error = _thread->getAddressSpace()->map(frigg::move(view),
					0, 0, 0x1000,
					AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead,
					&clientFileTable);
			assert(error == Error::success);
		}

		coroutine<void> runPosixRequests(LaneHandle lane);
		coroutine<void> runObserveLoop();

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
		frigg::SharedPtr<MemoryView> fileTableMemory;
		VirtualAddr clientFileTable;
	};

	coroutine<void> Process::runPosixRequests(LaneHandle lane) {
		while(true) {
			auto [acceptError, conversation] = co_await AcceptSender{lane};
			if(acceptError != Error::success) {
				frigg::infoLogger() << "thor: Could not accept POSIX lane" << frigg::endLog;
				co_return;
			}
			auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
			if(reqError != Error::success) {
				frigg::infoLogger() << "thor: Could not receive POSIX request" << frigg::endLog;
				co_return;
			}

			auto preamble = bragi::read_preamble(reqBuffer);
			assert(!preamble.error());

			if(preamble.id() == bragi::message_id<managarm::posix::GetTidRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::GetTidRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					frigg::infoLogger() << "thor: Could not parse POSIX request" << frigg::endLog;
					co_return;
				}

				posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_pid(1);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(preamble.id() == bragi::message_id<managarm::posix::OpenAtRequest>) {
				auto [tailError, tailBuffer] = co_await RecvBufferSender{conversation};
				if(tailError != Error::success) {
					frigg::infoLogger() << "thor: Could not receive POSIX tail" << frigg::endLog;
					co_return;
				}

				auto req = bragi::parse_head_tail<managarm::posix::OpenAtRequest>(
						reqBuffer, tailBuffer, *kernelAlloc);
				if(!req) {
					frigg::infoLogger() << "thor: Could not parse POSIX request" << frigg::endLog;
					co_return;
				}
				if(req->fd() != -100) {
					frigg::infoLogger() << "thor: OpenAt does not support dirfds" << frigg::endLog;
					co_return;
				}

				auto module = resolveModule(req->path());
				if(!module) {
					posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
					continue;
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

					auto fd = attachFile(file);

					posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::SUCCESS);
					resp.set_fd(fd);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
				}else{
					assert(module->type == MfsType::regular);

					auto stream = createStream();
					auto file = frigg::construct<ModuleFile>(*kernelAlloc,
							static_cast<MfsRegular *>(module));
					file->clientLane = frigg::move(stream.get<1>());

					auto closure = frigg::construct<initrd::FileRequestClosure>(*kernelAlloc,
							frigg::move(stream.get<0>()), file);
					(*closure)();

					auto fd = attachFile(file);

					posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::SUCCESS);
					resp.set_fd(fd);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
				}
			}else if(preamble.id() == bragi::message_id<managarm::posix::IsTtyRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::IsTtyRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					frigg::infoLogger() << "thor: Could not parse POSIX request" << frigg::endLog;
					co_return;
				}

				assert((size_t)req->fd() < openFiles.size());
				auto file = openFiles[req->fd()];

				posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_mode(file->isTerminal ? 1 : 0);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(preamble.id() == bragi::message_id<managarm::posix::CloseRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::CloseRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					frigg::infoLogger() << "thor: Could not parse POSIX request" << frigg::endLog;
					co_return;
				}

				// TODO: for now we just ignore close requests.
				posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(preamble.id() == bragi::message_id<managarm::posix::VmMapRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::VmMapRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					frigg::infoLogger() << "thor: Could not parse POSIX request" << frigg::endLog;
					co_return;
				}

				if(!req->size()) {
					posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
					continue;
				}

				if(!(req->flags() & 4)) // MAP_FIXED.
					assert(!"TODO: implement non-fixed mappings");

				uint32_t protFlags = 0;
				if(req->mode() & 1)
					protFlags |= AddressSpace::kMapProtRead;
				if(req->mode() & 2)
					protFlags |= AddressSpace::kMapProtWrite;
				if(req->mode() & 4)
					protFlags |= AddressSpace::kMapProtExecute;

				frigg::SharedPtr<MemoryView> fileMemory;
				if(req->flags() & 8) { // MAP_ANONYMOUS.
					// TODO: Use some always-zero memory for private anonymous mappings.
					fileMemory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, req->size());
				}else{
					// TODO: improve error handling here.
					assert((size_t)req->fd() < openFiles.size());
					auto abstractFile = openFiles[req->fd()];
					auto moduleFile = static_cast<ModuleFile *>(abstractFile);
					fileMemory = moduleFile->module->getMemory();
				}

				frigg::SharedPtr<MemorySlice> slice;
				if(req->flags() & 1) { // MAP_PRIVATE.
					auto cowMemory = frigg::makeShared<CopyOnWriteMemory>(*kernelAlloc,
							std::move(fileMemory), req->rel_offset(), req->size());
					slice = frigg::makeShared<MemorySlice>(*kernelAlloc,
							std::move(cowMemory), 0, req->size());
				}else{
					assert(!"TODO: implement shared mappings");
				}

				VirtualAddr address;
				auto space = _thread->getAddressSpace();
				auto error = space->map(std::move(slice),
						req->address_hint(), 0, req->size(),
						AddressSpace::kMapFixed | protFlags,
						&address);
				// TODO: improve error handling here.
				assert(error == Error::success);

				posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_offset(address);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frigg::UniqueMemory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else{
				frigg::infoLogger() << "thor: Illegal POSIX request type "
						<< preamble.id() << frigg::endLog;
				co_return;
			}
		}
	}

	coroutine<void> Process::runObserveLoop() {
		uint64_t currentSeq = 1;
		while(true) {
			auto [error, observedSeq, interrupt] = co_await _thread->observe(currentSeq);
			assert(error == Error::success);
			currentSeq = observedSeq;

			if(interrupt == kIntrPanic) {
				// Do nothing and stop observing.
				// TODO: Make sure the server is destructed here.
				frigg::infoLogger() << "\e[31m" "thor: Panic in server "
						<< name().data() << "\e[39m" << frigg::endLog;
				break;
			}else if(interrupt == kIntrPageFault) {
				// Do nothing and stop observing.
				// TODO: Make sure the server is destructed here.
				frigg::infoLogger() << "\e[31m" "thor: Fault in server "
						<< name().data() << "\e[39m" << frigg::endLog;
				break;
			}else if(interrupt == kIntrSuperCall + 10) { // ANON_ALLOCATE.
				// TODO: Use some always-zero memory for private anonymous mappings.
				auto size = _thread->_executor.general()->rsi;
				auto fileMemory = frigg::makeShared<AllocatedMemory>(*kernelAlloc, size);
				auto cowMemory = frigg::makeShared<CopyOnWriteMemory>(*kernelAlloc,
						std::move(fileMemory), 0, size);
				auto slice = frigg::makeShared<MemorySlice>(*kernelAlloc,
						std::move(cowMemory), 0, size);

				VirtualAddr address;
				auto space = _thread->getAddressSpace();
				auto error = space->map(std::move(slice),
						0, 0, size,
						AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead
						| AddressSpace::kMapProtWrite,
						&address);
				// TODO: improve error handling here.
				assert(error == Error::success);

				_thread->_executor.general()->rdi = kHelErrNone;
				_thread->_executor.general()->rsi = address;
				if(auto e = Thread::resumeOther(_thread); e != Error::success)
					frigg::panicLogger() << "thor: Failed to resume server" << frigg::endLog;
			}else if(interrupt == kIntrSuperCall + 11) { // ANON_FREE.
				auto address = _thread->_executor.general()->rsi;
				auto size = _thread->_executor.general()->rdx;
				auto space = _thread->getAddressSpace();
				co_await space->unmap(address, size);

				_thread->_executor.general()->rdi = kHelErrNone;
				_thread->_executor.general()->rsi = 0;
				if(auto e = Thread::resumeOther(_thread); e != Error::success)
					frigg::panicLogger() << "thor: Failed to resume server" << frigg::endLog;
			}else if(interrupt == kIntrSuperCall + 1) {
				ManagarmProcessData data = {
					kHelThisThread,
					nullptr,
					reinterpret_cast<HelHandle *>(clientFileTable),
					nullptr
				};

				{
					AddressSpaceLockHandle spaceLock{_thread->getAddressSpace().lock(),
							reinterpret_cast<void *>(_thread->_executor.general()->rsi),
							sizeof(ManagarmProcessData)};
					co_await spaceLock.acquire();

					spaceLock.write(0, &data, sizeof(ManagarmProcessData));
				}

				_thread->_executor.general()->rdi = kHelErrNone;
				if(auto e = Thread::resumeOther(_thread); e != Error::success)
					frigg::panicLogger() << "thor: Failed to resume server" << frigg::endLog;
			}else if(interrupt == kIntrSuperCall + 64) {
				AcquireNode node;

				ManagarmServerData data = {
					controlHandle
				};

				{
					AddressSpaceLockHandle spaceLock{_thread->getAddressSpace().lock(),
							reinterpret_cast<void *>(_thread->_executor.general()->rsi),
							sizeof(ManagarmServerData)};
					co_await spaceLock.acquire();

					spaceLock.write(0, &data, sizeof(ManagarmServerData));
				}

				_thread->_executor.general()->rdi = kHelErrNone;
				if(auto e = Thread::resumeOther(_thread); e != Error::success)
					frigg::panicLogger() << "thor: Failed to resume server" << frigg::endLog;
			}else if(interrupt == kIntrSuperCall + 7) { // sigprocmask.
				_thread->_executor.general()->rdi = kHelErrNone;
				_thread->_executor.general()->rsi = 0;
				if(auto e = Thread::resumeOther(_thread); e != Error::success)
					frigg::panicLogger() << "thor: Failed to resume server" << frigg::endLog;
			}else{
				frigg::panicLogger() << "thor: Unexpected observation "
						<< (uint32_t)interrupt << frigg::endLog;
			}
		}
	}
} // namepace initrd

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

		async::detach_with_allocator(*kernelAlloc,
				process->runPosixRequests(thread->superiorLane()));
		async::detach_with_allocator(*kernelAlloc,
				process->runObserveLoop());

		// Just block this fiber forever (we're still processing worklets).
		FiberBlocker blocker;
		blocker.setup();
		KernelFiber::blockCurrent(&blocker);
	});
}

} // namespace thor

