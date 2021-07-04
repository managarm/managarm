#include <bragi/helpers-frigg.hpp>
#include <frg/span.hpp>
#include <frg/string.hpp>
#include <fs.frigg_bragi.hpp>
#include <posix.frigg_bragi.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/gdbserver.hpp>
#include <thor-internal/module.hpp>
#include <thor-internal/stream.hpp>

namespace thor {

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
	coroutine<void> runStdioRequests(LaneHandle lane) {
		frg::string<KernelAlloc> lineBuffer{*kernelAlloc};

		while(true) {
			auto [acceptError, conversation] = co_await AcceptSender{lane};
			if(acceptError == Error::endOfLane)
				break;
			if(acceptError != Error::success) {
				infoLogger() << "thor: Could not accept stdio lane" << frg::endlog;
				co_return;
			}
			auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
			if(reqError != Error::success) {
				infoLogger() << "thor: Could not receive stdio request" << frg::endlog;
				co_return;
			}

			managarm::fs::CntRequest<KernelAlloc> req(*kernelAlloc);
			req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

			if(req.req_type() == managarm::fs::CntReqType::WRITE) {
				auto [credsError, credentials] = co_await ExtractCredentialsSender{conversation};
				if(credsError != Error::success) {
					infoLogger() << "thor: Could not receive stdio credentials"
							<< frg::endlog;
					co_return;
				}
				auto [dataError, dataBuffer] = co_await RecvBufferSender{conversation};
				if(dataError != Error::success) {
					infoLogger() << "thor: Could not receive stdio data" << frg::endlog;
					co_return;
				}

				for(size_t i = 0; i < dataBuffer.size(); i++) {
					auto c = reinterpret_cast<char *>(dataBuffer.data())[i];
					if(c == '\n') {
						infoLogger() << lineBuffer << frg::endlog;
						lineBuffer.resize(0);
					}else{
						lineBuffer += c;
					}
				}

				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SUCCESS);
				resp.set_size(dataBuffer.size());

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(req.req_type() == managarm::fs::CntReqType::SEEK_REL) {
				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SEEK_ON_PIPE);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else{
				infoLogger() << "\e[31m" "thor: Illegal request type " << (int32_t)req.req_type()
						<< " for kernel provided stdio file" "\e[39m" << frg::endlog;

				auto dismissError = co_await DismissSender{conversation};
				// TODO: improve error handling here.
				assert(dismissError == Error::success);
			}
		}
	}

} // namespace stdio

namespace initrd {
	struct OpenRegular : OpenFile {
		OpenRegular(MfsRegular *module)
		: module(module), offset(0) { }

		MfsRegular *module;
		size_t offset;
	};

	struct OpenDirectory : OpenFile {
		OpenDirectory(MfsDirectory *node)
		: node{node}, index(0) { }

		MfsDirectory *node;
		size_t index;
	};

	// ----------------------------------------------------
	// initrd file handling.
	// ----------------------------------------------------

	coroutine<void> runRegularRequests(OpenRegular *file, LaneHandle lane) {
		while(true) {
			auto [acceptError, conversation] = co_await AcceptSender{lane};
			if(acceptError == Error::endOfLane)
				break;
			if(acceptError != Error::success) {
				infoLogger() << "thor: Could not accept regular lane" << frg::endlog;
				co_return;
			}
			auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
			if(reqError != Error::success) {
				infoLogger() << "thor: Could not receive regular request" << frg::endlog;
				co_return;
			}

			managarm::fs::CntRequest<KernelAlloc> req(*kernelAlloc);
			req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

			if(req.req_type() == managarm::fs::CntReqType::READ) {
				auto [credsError, credentials] = co_await ExtractCredentialsSender{conversation};
				if(credsError != Error::success) {
					infoLogger() << "thor: Could not receive stdio credentials"
							<< frg::endlog;
					co_return;
				}

				frg::unique_memory<KernelAlloc> dataBuffer{*kernelAlloc,
						frg::min(size_t(req.size()), file->module->size() - file->offset)};
				co_await copyFromView(file->module->getMemory().get(), file->offset,
					dataBuffer.data(), dataBuffer.size(), WorkQueue::generalQueue()->take());
				file->offset += dataBuffer.size();

				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SUCCESS);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);

				auto dataError = co_await SendBufferSender{conversation, std::move(dataBuffer)};
				// TODO: improve error handling here.
				assert(dataError == Error::success);
			}else if(req.req_type() == managarm::fs::CntReqType::SEEK_ABS) {
				file->offset = req.rel_offset();

				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SUCCESS);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(req.req_type() == managarm::fs::CntReqType::MMAP) {
				managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::fs::Errors::SUCCESS);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);

				auto memoryError = co_await PushDescriptorSender{conversation,
						MemoryViewDescriptor{file->module->getMemory()}};
				// TODO: improve error handling here.
				assert(memoryError == Error::success);
			}else{
				infoLogger() << "\e[31m" "thor: Illegal request type " << (int32_t)req.req_type()
						<< " for kernel provided regular file" "\e[39m" << frg::endlog;

				auto dismissError = co_await DismissSender{conversation};
				// TODO: improve error handling here.
				assert(dismissError == Error::success);
			}

		}
	}

	coroutine<void> runDirectoryRequests(OpenDirectory *file, LaneHandle lane) {
		while(true) {
			auto [acceptError, conversation] = co_await AcceptSender{lane};
			if(acceptError == Error::endOfLane)
				break;
			if(acceptError != Error::success) {
				infoLogger() << "thor: Could not accept directory lane" << frg::endlog;
				co_return;
			}
			auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
			if(reqError != Error::success) {
				infoLogger() << "thor: Could not receive directory request" << frg::endlog;
				co_return;
			}

			managarm::fs::CntRequest<KernelAlloc> req(*kernelAlloc);
			req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

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
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
				}else{
					managarm::fs::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::fs::Errors::END_OF_FILE);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
				}
			}else{
				infoLogger() << "\e[31m" "thor: Illegal request type " << (int32_t)req.req_type()
						<< " for kernel provided directory file" "\e[39m" << frg::endlog;

				auto dismissError = co_await DismissSender{conversation};
				// TODO: improve error handling here.
				assert(dismissError == Error::success);
			}
		}
	}
} // namepace initrd

namespace posix {
	// ----------------------------------------------------
	// POSIX server.
	// ----------------------------------------------------

	struct Process {
		Process(frg::string<KernelAlloc> name, smarter::shared_ptr<Thread, ActiveHandle> thread)
		: _name{std::move(name)}, _thread(std::move(thread)), openFiles(*kernelAlloc) {
			fileTableMemory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, 0x1000);
			fileTableMemory->selfPtr = fileTableMemory;

			auto posixStream = createStream();
			posixLane = std::move(posixStream.get<0>());

			auto irqLock = frg::guard(&irqMutex());
			Universe::Guard universeLock(_thread->getUniverse()->lock);

			posixHandle = _thread->getUniverse()->attachDescriptor(universeLock,
					LaneDescriptor{std::move(posixStream.get<1>())});
		}

		coroutine<void> setupAddressSpace() {
			auto view = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
					fileTableMemory, 0, 0x1000);
			auto result = co_await _thread->getAddressSpace()->map(std::move(view),
					0, 0, 0x1000,
					AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead);
			assert(result);
			clientFileTable = result.value();
		}

		coroutine<void> runPosixRequests();
		coroutine<void> runObserveLoop();

		frg::string_view name() {
			return _name;
		}

		void attachControl(LaneHandle lane) {
			auto irq_lock = frg::guard(&irqMutex());
			Universe::Guard universe_guard(_thread->getUniverse()->lock);

			controlHandle = _thread->getUniverse()->attachDescriptor(universe_guard,
					LaneDescriptor{lane});
		}

		coroutine<int> attachFile(OpenFile *file) {
			Handle handle;
			{
				auto irq_lock = frg::guard(&irqMutex());
				Universe::Guard universe_guard(_thread->getUniverse()->lock);

				handle = _thread->getUniverse()->attachDescriptor(universe_guard,
						LaneDescriptor(file->clientLane));
			}

			for(int fd = 0; fd < (int)openFiles.size(); ++fd) {
				if(openFiles[fd])
					continue;
				openFiles[fd] = file;
				co_await copyToView(fileTableMemory.get(), sizeof(Handle) * fd,
						&handle, sizeof(Handle), WorkQueue::generalQueue()->take());
				co_return fd;
			}

			int fd = openFiles.size();
			openFiles.push(file);
			co_await copyToView(fileTableMemory.get(), sizeof(Handle) * fd,
					&handle, sizeof(Handle), WorkQueue::generalQueue()->take());
			co_return fd;
		}

		frg::string<KernelAlloc> _name;
		smarter::shared_ptr<Thread, ActiveHandle> _thread;

		Handle posixHandle;
		Handle controlHandle;
		LaneHandle posixLane;
		frg::vector<OpenFile *, KernelAlloc> openFiles;
		smarter::shared_ptr<AllocatedMemory> fileTableMemory;
		VirtualAddr clientFileTable;
	};

	coroutine<void> Process::runPosixRequests() {
		while(true) {
			auto [acceptError, conversation] = co_await AcceptSender{posixLane};
			if(acceptError != Error::success) {
				infoLogger() << "thor: Could not accept POSIX lane" << frg::endlog;
				co_return;
			}
			auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
			if(reqError != Error::success) {
				infoLogger() << "thor: Could not receive POSIX request" << frg::endlog;
				co_return;
			}

			auto preamble = bragi::read_preamble(reqBuffer);
			assert(!preamble.error());

			if(preamble.id() == bragi::message_id<managarm::posix::CntRequest>) {
				// This case is only really needed to return an error from SIG_ACTION,
				// since mlibc tries to install a signal handler to support cancellation.

				auto req = bragi::parse_head_only<managarm::posix::CntRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}

				if(req->request_type() != managarm::posix::CntReqType::SIG_ACTION)
					infoLogger() << "thor: Unexpected legacy POSIX request "
							<< req->request_type() << frg::endlog;

				managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(preamble.id() == bragi::message_id<managarm::posix::GetTidRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::GetTidRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}

				managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_pid(1);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(preamble.id() == bragi::message_id<managarm::posix::OpenAtRequest>) {
				auto [tailError, tailBuffer] = co_await RecvBufferSender{conversation};
				if(tailError != Error::success) {
					infoLogger() << "thor: Could not receive POSIX tail" << frg::endlog;
					co_return;
				}

				auto req = bragi::parse_head_tail<managarm::posix::OpenAtRequest>(
						reqBuffer, tailBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}
				if(req->fd() != -100) {
					infoLogger() << "thor: OpenAt does not support dirfds" << frg::endlog;
					co_return;
				}

				auto module = resolveModule(req->path());
				if(!module) {
					managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
					continue;
				}

				if(module->type == MfsType::directory) {
					auto stream = createStream();
					auto file = frg::construct<initrd::OpenDirectory>(*kernelAlloc,
							static_cast<MfsDirectory *>(module));
					file->clientLane = std::move(stream.get<1>());

					async::detach_with_allocator(*kernelAlloc,
							runDirectoryRequests(file, std::move(stream.get<0>())));

					auto fd = co_await attachFile(file);

					managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::SUCCESS);
					resp.set_fd(fd);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
				}else{
					assert(module->type == MfsType::regular);

					auto stream = createStream();
					auto file = frg::construct<initrd::OpenRegular>(*kernelAlloc,
							static_cast<MfsRegular *>(module));
					file->clientLane = std::move(stream.get<1>());

					async::detach_with_allocator(*kernelAlloc,
							runRegularRequests(file, std::move(stream.get<0>())));

					auto fd = co_await attachFile(file);

					managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::SUCCESS);
					resp.set_fd(fd);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
					memcpy(respBuffer.data(), ser.data(), ser.size());
					auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
					// TODO: improve error handling here.
					assert(respError == Error::success);
				}
			}else if(preamble.id() == bragi::message_id<managarm::posix::IsTtyRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::IsTtyRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}

				assert((size_t)req->fd() < openFiles.size());
				auto file = openFiles[req->fd()];

				managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_mode(file->isTerminal ? 1 : 0);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(preamble.id() == bragi::message_id<managarm::posix::CloseRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::CloseRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}

				// TODO: for now we just ignore close requests.
				managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else if(preamble.id() == bragi::message_id<managarm::posix::VmMapRequest>) {
				auto req = bragi::parse_head_only<managarm::posix::VmMapRequest>(
						reqBuffer, *kernelAlloc);
				if(!req) {
					infoLogger() << "thor: Could not parse POSIX request" << frg::endlog;
					co_return;
				}

				if(!req->size()) {
					managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
					resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

					frg::string<KernelAlloc> ser(*kernelAlloc);
					resp.SerializeToString(&ser);
					frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
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

				smarter::shared_ptr<MemoryView> fileMemory;
				if(req->flags() & 8) { // MAP_ANONYMOUS.
					// TODO: Use some always-zero memory for private anonymous mappings.
					auto memory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, req->size());
					memory->selfPtr = memory;
					fileMemory = std::move(memory);
				}else{
					// TODO: improve error handling here.
					assert((size_t)req->fd() < openFiles.size());
					auto abstractFile = openFiles[req->fd()];
					auto moduleFile = static_cast<initrd::OpenRegular *>(abstractFile);
					fileMemory = moduleFile->module->getMemory();
				}

				smarter::shared_ptr<MemorySlice> slice;
				if(req->flags() & 1) { // MAP_PRIVATE.
					auto cowMemory = smarter::allocate_shared<CopyOnWriteMemory>(*kernelAlloc,
							std::move(fileMemory), req->rel_offset(), req->size());
					cowMemory->selfPtr = cowMemory;
					slice = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
							std::move(cowMemory), 0, req->size());
				}else{
					assert(!"TODO: implement shared mappings");
				}

				auto space = _thread->getAddressSpace();
				auto mapResult = co_await space->map(std::move(slice),
						req->address_hint(), 0, req->size(),
						AddressSpace::kMapFixed | protFlags);
				// TODO: improve error handling here.
				assert(mapResult);

				managarm::posix::SvrResponse<KernelAlloc> resp(*kernelAlloc);
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_offset(mapResult.value());

				frg::string<KernelAlloc> ser(*kernelAlloc);
				resp.SerializeToString(&ser);
				frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
				memcpy(respBuffer.data(), ser.data(), ser.size());
				auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
				// TODO: improve error handling here.
				assert(respError == Error::success);
			}else{
				infoLogger() << "thor: Illegal POSIX request type "
						<< preamble.id() << frg::endlog;
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
				infoLogger() << "\e[31m" "thor: Panic in server "
						<< name().data() << "\e[39m" << frg::endlog;
				launchGdbServer(_thread, _name, WorkQueue::generalQueue()->take());
				break;
			}else if(interrupt == kIntrPageFault) {
				// Do nothing and stop observing.
				// TODO: Make sure the server is destructed here.
				infoLogger() << "\e[31m" "thor: Fault in server "
						<< name().data() << "\e[39m" << frg::endlog;
				launchGdbServer(_thread, _name, WorkQueue::generalQueue()->take());
				break;
			}else if(interrupt == kIntrSuperCall + 10) { // ANON_ALLOCATE.
				// TODO: Use some always-zero memory for private anonymous mappings.
				auto size = *_thread->_executor.arg0();
				auto fileMemory = smarter::allocate_shared<AllocatedMemory>(*kernelAlloc, size);
				fileMemory->selfPtr = fileMemory;
				auto cowMemory = smarter::allocate_shared<CopyOnWriteMemory>(*kernelAlloc,
						std::move(fileMemory), 0, size);
				cowMemory->selfPtr = cowMemory;
				auto slice = smarter::allocate_shared<MemorySlice>(*kernelAlloc,
						std::move(cowMemory), 0, size);

				auto space = _thread->getAddressSpace();
				auto mapResult = co_await space->map(std::move(slice),
						0, 0, size,
						AddressSpace::kMapPreferTop | AddressSpace::kMapProtRead
						| AddressSpace::kMapProtWrite);
				// TODO: improve error handling here.
				assert(mapResult);

				*_thread->_executor.result0() = kHelErrNone;
				*_thread->_executor.result1() = mapResult.value();
				if(auto e = Thread::resumeOther(remove_tag_cast(_thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else if(interrupt == kIntrSuperCall + 11) { // ANON_FREE.
				auto address = *_thread->_executor.arg0();
				auto size = *_thread->_executor.arg1();
				auto space = _thread->getAddressSpace();
				auto unmapOutcome = co_await space->unmap(address, size);

				if(!unmapOutcome) {
					assert(unmapOutcome.error() == Error::illegalArgs);
					*_thread->_executor.result0() = kHelErrIllegalArgs;
				}else{
					*_thread->_executor.result0() = kHelErrNone;
				}
				*_thread->_executor.result1() = 0;
				if(auto e = Thread::resumeOther(remove_tag_cast(_thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else if(interrupt == kIntrSuperCall + 1) {
				ManagarmProcessData data = {
					posixHandle,
					nullptr,
					reinterpret_cast<HelHandle *>(clientFileTable),
					nullptr
				};

				auto outcome = co_await _thread->getAddressSpace()->writeSpace(
						*_thread->_executor.arg0(), &data, sizeof(ManagarmProcessData),
						WorkQueue::generalQueue()->take());
				if(!outcome) {
					*_thread->_executor.result0() = kHelErrFault;
				}else{
					*_thread->_executor.result0() = kHelErrNone;
				}
				if(auto e = Thread::resumeOther(remove_tag_cast(_thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else if(interrupt == kIntrSuperCall + 64) {
				ManagarmServerData data = {
					controlHandle
				};

				auto outcome = co_await _thread->getAddressSpace()->writeSpace(
						*_thread->_executor.arg0(), &data, sizeof(ManagarmServerData),
						WorkQueue::generalQueue()->take());
				if(!outcome) {
					*_thread->_executor.result0() = kHelErrFault;
				}else{
					*_thread->_executor.result0() = kHelErrNone;
				}
				if(auto e = Thread::resumeOther(remove_tag_cast(_thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else if(interrupt == kIntrSuperCall + 7) { // sigprocmask.
				*_thread->_executor.result0() = kHelErrNone;
				*_thread->_executor.result1() = 0;
				if(auto e = Thread::resumeOther(remove_tag_cast(_thread)); e != Error::success)
					panicLogger() << "thor: Failed to resume server" << frg::endlog;
			}else{
				panicLogger() << "thor: Unexpected observation "
						<< (uint32_t)interrupt << frg::endlog;
			}
		}
	}
} // namepace posix

void runService(frg::string<KernelAlloc> name, LaneHandle controlLane,
		smarter::shared_ptr<Thread, ActiveHandle> thread) {
	KernelFiber::run([name, thread, controlLane = std::move(controlLane)] () mutable {
		auto stdioStream = createStream();
		auto stdioFile = frg::construct<StdioFile>(*kernelAlloc);
		stdioFile->clientLane = std::move(stdioStream.get<1>());

		async::detach_with_allocator(*kernelAlloc,
				stdio::runStdioRequests(stdioStream.get<0>()));

		auto process = frg::construct<posix::Process>(*kernelAlloc, std::move(name), thread);
		KernelFiber::asyncBlockCurrent(process->setupAddressSpace());
		process->attachControl(std::move(controlLane));
		KernelFiber::asyncBlockCurrent(process->attachFile(stdioFile));
		KernelFiber::asyncBlockCurrent(process->attachFile(stdioFile));
		KernelFiber::asyncBlockCurrent(process->attachFile(stdioFile));

		async::detach_with_allocator(*kernelAlloc,
				process->runPosixRequests());
		async::detach_with_allocator(*kernelAlloc,
				process->runObserveLoop());

		// Just block this fiber forever (we're still processing worklets).
		FiberBlocker blocker;
		blocker.setup();
		KernelFiber::blockCurrent(&blocker);
	});
}

} // namespace thor
