#include <format>
#include <print>
#include <limits.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/pidfd.h>
#include <unistd.h>
#include <print>

#include "epoll.hpp"
#include "fifo.hpp"
#include "signalfd.hpp"

#include <bragi/helpers-std.hpp>
#include <posix.bragi.hpp>
#include <kerncfg.bragi.hpp>

#include "clocks.hpp"
#include "debug-options.hpp"
#include "requests/common.hpp"

async::result<void> serveRequests(std::shared_ptr<Process> self,
		std::shared_ptr<Generation> generation) {
	auto logRequest = [&self]<class... Args>(bool cond, std::string_view name,
	std::format_string<Args...> fmt = {""}, Args&&... args) {
		if(cond)
			std::cout << "posix: [" << self->pid() << "] " << name << " "
			          << std::format(fmt, std::forward<Args>(args)...) << std::endl;
	};

	async::cancellation_token cancellation = generation->cancelServe;

	async::cancellation_callback cancel_callback{cancellation, [&] {
		HEL_CHECK(helShutdownLane(self->posixLane().getHandle()));
	}};

	while(true) {
		auto [accept, recv_head] = co_await helix_ng::exchangeMsgs(
				self->posixLane(),
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		protocols::ostrace::Timer timer;

		if(accept.error() == kHelErrLaneShutdown)
			break;
		HEL_CHECK(accept.error());

		if(recv_head.error() == kHelErrBufferTooSmall) {
			std::cout << "posix: Rejecting request due to RecvInline overflow" << std::endl;
			continue;
		}
		HEL_CHECK(recv_head.error());

		auto conversation = accept.descriptor();

		auto preamble = bragi::read_preamble(recv_head);
		assert(!preamble.error());
		recv_head.reset();

		timespec requestTimestamp = {};
		auto logBragiRequest = [&self, &recv_head, &requestTimestamp](std::span<uint8_t> tail) {
			if(!posix::ostContext.isActive())
				return;

			requestTimestamp = clk::getTimeSinceBoot();
			posix::ostContext.emitWithTimestamp(
				posix::ostEvtRequest,
				(requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec,
				posix::ostAttrPid(self->tid()),
				posix::ostAttrTime((requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec),
				posix::ostBragi(std::span<uint8_t>{reinterpret_cast<uint8_t *>(recv_head.data()), recv_head.size()}, tail)
			);
		};

		auto logBragiReply = [&self, &preamble, &requestTimestamp](auto &resp) {
			if(!posix::ostContext.isActive())
				return;

			auto ts = clk::getTimeSinceBoot();
			std::string replyHead;
			std::string replyTail;
			replyHead.resize(resp.size_of_head());
			replyTail.resize(resp.size_of_tail());
			bragi::limited_writer headWriter{replyHead.data(), replyHead.size()};
			bragi::limited_writer tailWriter{replyTail.data(), replyTail.size()};
			auto headOk = resp.encode_head(headWriter);
			auto tailOk = resp.encode_tail(tailWriter);
			assert(headOk);
			assert(tailOk);
			posix::ostContext.emitWithTimestamp(
				posix::ostEvtRequest,
				(ts.tv_sec * 1'000'000'000) + ts.tv_nsec,
				posix::ostAttrRequest(preamble.id()),
				posix::ostAttrTime((requestTimestamp.tv_sec * 1'000'000'000) + requestTimestamp.tv_nsec),
				posix::ostAttrPid(self->tid()),
				posix::ostBragi({reinterpret_cast<uint8_t *>(replyHead.data()), replyHead.size()}, {reinterpret_cast<uint8_t *>(replyTail.data()), replyTail.size()})
			);
		};

		auto sendErrorResponse = [&]<typename Message = managarm::posix::SvrResponse>(managarm::posix::Errors err) -> async::result<void> {
			Message resp;
			resp.set_error(err);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		};

		if(!preamble.tail_size())
			logBragiRequest({});

		managarm::posix::CntRequest req;
		if (preamble.id() == managarm::posix::CntRequest::message_id) {
			auto o = bragi::parse_head_only<managarm::posix::CntRequest>(recv_head);
			if (!o) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			req = *o;
		}

		#define MAKE_CASE(id) case managarm::posix::id##Request::message_id: handler = &requests::handle##id; break;

		requests::RequestHandler handler = nullptr;
		switch (preamble.id()) {
		// From fd.cpp
		MAKE_CASE(Dup2)
		MAKE_CASE(IsTty)
		MAKE_CASE(IoctlFioclex)
		MAKE_CASE(Close)
		// From filesystem.cpp
		MAKE_CASE(Chroot)
		MAKE_CASE(Chdir)
		MAKE_CASE(AccessAt)
		MAKE_CASE(MkdirAt)
		MAKE_CASE(MkfifoAt)
		MAKE_CASE(LinkAt)
		MAKE_CASE(SymlinkAt)
		MAKE_CASE(ReadlinkAt)
		MAKE_CASE(RenameAt)
		MAKE_CASE(UnlinkAt)
		MAKE_CASE(Rmdir)
		MAKE_CASE(FstatAt)
		MAKE_CASE(Fstatfs)
		MAKE_CASE(FchmodAt)
		MAKE_CASE(UtimensAt)
		MAKE_CASE(OpenAt)
		MAKE_CASE(MknodAt)
		MAKE_CASE(Umask)
		// From special-files.cpp
		MAKE_CASE(InotifyCreate)
		MAKE_CASE(InotifyAdd)
		MAKE_CASE(InotifyRm)
		MAKE_CASE(EventfdCreate)
		MAKE_CASE(TimerFdCreate)
		MAKE_CASE(TimerFdSet)
		MAKE_CASE(TimerFdGet)
		MAKE_CASE(PidfdOpen)
		MAKE_CASE(PidfdSendSignal)
		MAKE_CASE(PidfdGetPid)
		// From memory.cpp
		MAKE_CASE(VmMap)
		MAKE_CASE(MemFdCreate)
		// From uid-gid.cpp
		MAKE_CASE(GetPid)
		MAKE_CASE(GetPpid)
		MAKE_CASE(GetUid)
		MAKE_CASE(SetUid)
		MAKE_CASE(GetEuid)
		MAKE_CASE(SetEuid)
		MAKE_CASE(GetGid)
		MAKE_CASE(GetEgid)
		MAKE_CASE(SetGid)
		MAKE_CASE(SetEgid)
		// From process.cpp
		MAKE_CASE(WaitId)
		MAKE_CASE(SetAffinity)
		MAKE_CASE(GetAffinity)
		MAKE_CASE(GetPgid)
		MAKE_CASE(SetPgid)
		MAKE_CASE(GetSid)
		MAKE_CASE(ParentDeathSignal)
		MAKE_CASE(ProcessDumpable)
		MAKE_CASE(SetResourceLimit)
		// From socket.cpp
		MAKE_CASE(Netserver)
		MAKE_CASE(Socket)
		MAKE_CASE(Sockpair)
		MAKE_CASE(Accept)
		// From system.cpp
		MAKE_CASE(Reboot)
		MAKE_CASE(Mount)
		MAKE_CASE(Sysconf)
		MAKE_CASE(GetMemoryInformation)
		// From timer.cpp
		MAKE_CASE(SetIntervalTimer)
		MAKE_CASE(TimerCreate)
		MAKE_CASE(TimerSet)
		MAKE_CASE(TimerGet)
		MAKE_CASE(TimerDelete)
		default:
			// Do nothing.
		}

		// Try the new dispatch system first
		if (handler) {
			// Build request context (some fields passed by reference to avoid copying non-copyable types)
			requests::RequestContext ctx{
				self,
				generation,
				conversation,
				preamble,
				recv_head,
				requestTimestamp,
				timer
			};
			
			// Call the handler
			co_await handler(ctx);
			
			// Emit ostrace event
			if(posix::ostContext.isActive()) {
				posix::ostContext.emit(
					posix::ostEvtRequest,
					posix::ostAttrRequest(preamble.id()),
					posix::ostAttrTime(timer.elapsed())
				);
			}
			continue;
		}

		// Fallback to the old dispatch system for CntReq messages
		if(req.request_type() == managarm::posix::CntReqType::WAIT) {
			if(req.flags() & ~(WNOHANG | WUNTRACED | WCONTINUED)) {
				std::cout << "posix: WAIT invalid flags: " << req.flags() << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			WaitFlags flags = waitExited;

			if(req.flags() & WNOHANG)
				flags |= waitNonBlocking;

			if(req.flags() & WUNTRACED)
				std::cout << "\e[31mposix: WAIT flag WUNTRACED is silently ignored\e[39m" << std::endl;

			if(req.flags() & WCONTINUED)
				std::cout << "\e[31mposix: WAIT flag WCONTINUED is silently ignored\e[39m" << std::endl;

			logRequest(logRequests, "WAIT", "pid={}", req.pid());

			frg::expected<Error, Process::WaitResult> waitResult = Error::ioError;

			{
				auto cancelEvent = self->cancelEventRegistry().event(self->credentials(), req.cancellation_id());
				if (!cancelEvent) {
					std::println("posix: possibly duplicate cancellation ID registered");
					sendErrorResponse(managarm::posix::Errors::INTERNAL_ERROR);
					continue;
				}

				waitResult = co_await self->wait(req.pid(), flags, cancelEvent);
			}

			managarm::posix::SvrResponse resp;
			if(waitResult) {
				auto proc_state = waitResult.value();
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_pid(proc_state.pid);
				resp.set_ru_user_time(proc_state.stats.userTime);

				uint32_t mode = 0;
				if(auto byExit = std::get_if<TerminationByExit>(&proc_state.state); byExit) {
					mode |= W_EXITCODE(byExit->code, 0);
				}else if(auto bySignal = std::get_if<TerminationBySignal>(&proc_state.state); bySignal) {
					mode |= W_EXITCODE(0, bySignal->signo);
				}else{
					assert(std::holds_alternative<std::monostate>(proc_state.state));
				}
				resp.set_mode(mode);
			} else if (waitResult.error() == Error::interrupted) {
				resp.set_error(managarm::posix::Errors::INTERRUPTED);
			} else if (waitResult.error() == Error::wouldBlock) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_pid(0);
			} else {
				resp.set_error(waitResult.error() | toPosixProtoError);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::GET_RESOURCE_USAGE) {
			logRequest(logRequests, "GET_RESOURCE_USAGE");

			HelThreadStats stats;
			HEL_CHECK(helQueryThreadStats(self->threadDescriptor().getHandle(), &stats));

			int32_t mode = static_cast<int32_t>(req.mode());
			uint64_t user_time;
			if(mode == RUSAGE_SELF) {
				user_time = stats.userTime;
			}else if(mode == RUSAGE_CHILDREN) {
				user_time = self->threadGroup()->accumulatedUsage().userTime;
			}else{
				std::cout << "\e[31mposix: GET_RESOURCE_USAGE mode is not supported\e[39m"
						<< std::endl;
				user_time = 0;
				// TODO: Return an error response.
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_ru_user_time(user_time);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::VM_REMAP) {
			logRequest(logRequests, "VM_REMAP");

			auto address = co_await self->vmContext()->remapFile(
					reinterpret_cast<void *>(req.address()), req.size(), req.new_size());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_offset(reinterpret_cast<uintptr_t>(address));

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::VM_PROTECT) {
			logRequest(logRequests, "VM_PROTECT");
			managarm::posix::SvrResponse resp;

			if(req.mode() & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				continue;
			}

			uint32_t native_flags = 0;
			if(req.mode() & PROT_READ)
				native_flags |= kHelMapProtRead;
			if(req.mode() & PROT_WRITE)
				native_flags |= kHelMapProtWrite;
			if(req.mode() & PROT_EXEC)
				native_flags |= kHelMapProtExecute;

			co_await self->vmContext()->protectFile(
					reinterpret_cast<void *>(req.address()), req.size(), native_flags);

			resp.set_error(managarm::posix::Errors::SUCCESS);
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::VM_UNMAP) {
			logRequest(logRequests, "VM_UNMAP", "address={:#08x} size={:#x}", req.address(), req.size());

			size_t size = req.size();

			// Fail if address is not page-aligned or if the size is zero.
			if(req.address() & 0xFFF || size == 0) {
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				logBragiReply(resp);
				continue;
			}

			// Align size to page size.
			if(size & 0xFFF) {
				size = (size + 0xFFF) & ~0xFFF;
			}

			self->vmContext()->unmapFile(reinterpret_cast<void *>(req.address()), size);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::FCHDIR) {
			logRequest(logRequests, "FCHDIR");

			managarm::posix::SvrResponse resp;

			auto file = self->fileContext()->getFile(req.fd());

			if(!file) {
				resp.set_error(managarm::posix::Errors::NO_SUCH_FD);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				continue;
			}

			self->fsContext()->changeWorkingDirectory({file->associatedMount(),
					file->associatedLink()});

			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::DUP) {
			logRequest(logRequests, "DUP", "fd={}", req.fd());

			auto file = self->fileContext()->getFile(req.fd());

			if (!file) {
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::NO_SUCH_FD);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				logBragiReply(resp);
				continue;
			}

			if(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC)) {
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				logBragiReply(resp);
				continue;
			}

			auto newfd = self->fileContext()->attachFile(file,
					req.flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

			managarm::posix::SvrResponse resp;
			if (newfd) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(newfd.value());
			} else {
				resp.set_error(newfd.error() | toPosixProtoError);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
		}else if(req.request_type() == managarm::posix::CntReqType::TTY_NAME) {
			logRequest(logRequests, "TTY_NAME", "fd={}", req.fd());

			std::cout << "\e[31mposix: Fix TTY_NAME\e[39m" << std::endl;
			managarm::posix::SvrResponse resp;

			auto file = self->fileContext()->getFile(req.fd());
			if(!file) {
			    co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
			    continue;
			}

			auto ttynameResult = co_await file->ttyname();
			if(!ttynameResult) {
			    assert(ttynameResult.error() == Error::notTerminal);
			    co_await sendErrorResponse(managarm::posix::Errors::NOT_A_TTY);
			    continue;
			}

			resp.set_path(ttynameResult.value());
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::GETCWD) {
			std::string path = self->fsContext()->getWorkingDirectory().getPath(
					self->fsContext()->getRoot());

			logRequest(logRequests, "GETCWD", "path={}", path);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_size(path.size());

			auto [send_resp, send_path] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(path.data(), std::min(static_cast<size_t>(req.size()), path.size() + 1))
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_path.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::FD_GET_FLAGS) {
			logRequest(logRequests, "FD_GET_FLAGS");

			auto descriptor = self->fileContext()->getDescriptor(req.fd());
			if(!descriptor) {
				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::NO_SUCH_FD);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				logBragiReply(resp);
				continue;
			}

			int flags = 0;
			if(descriptor->closeOnExec)
				flags |= FD_CLOEXEC;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_flags(flags);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::FD_SET_FLAGS) {
			logRequest(logRequests, "FD_SET_FLAGS");

			if(req.flags() & ~FD_CLOEXEC) {
				std::cout << "posix: FD_SET_FLAGS unknown flags: " << req.flags() << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}
			int closeOnExec = req.flags() & FD_CLOEXEC;
			if(self->fileContext()->setDescriptor(req.fd(), closeOnExec) != Error::success) {
				co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
				continue;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::SIG_ACTION) {
			logRequest(logRequests, "SIG_ACTION");

			if(req.flags() & ~(SA_ONSTACK | SA_SIGINFO | SA_RESETHAND | SA_NODEFER | SA_RESTART | SA_NOCLDSTOP | SA_NOCLDWAIT)) {
				std::cout << "\e[31mposix: Unknown SIG_ACTION flags: 0x"
						<< std::hex << req.flags()
						<< std::dec << "\e[39m" << std::endl;
				assert(!"Flags not implemented");
			}

			managarm::posix::SvrResponse resp;

			if(req.sig_number() <= 0 || req.sig_number() > 64) {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
				HEL_CHECK(send_resp.error());
				logBragiReply(resp);
				continue;
			}

			auto removePendingSignal = [&](int signo) -> async::result<void> {
				if (self->delayedSignal && self->delayedSignal->signalNumber == static_cast<int>(signo)) {
					// If there is a pending signal that is now being ignored, remove it.
					delete self->delayedSignal;
					self->delayedSignal = nullptr;
					self->delayedSignalHandling = std::nullopt;
				}

				while (co_await self->threadGroup()->signalContext()->fetchSignal(1 << (signo - 1), true));
			};

			std::set<int> defaultIgnoredSignals = {SIGCHLD, SIGURG, SIGWINCH};

			SignalHandler saved_handler;
			if(req.mode()) {
				SignalHandler handler;
				if(req.sig_handler() == uintptr_t(SIG_DFL)) {
					handler.disposition = SignalDisposition::none;
					// POSIX requires discarding pending signals when setting SIG_DFL for signals,
					// if their default action is to ignore (POSIX 2024, B.2.4.3 Signal Actions)
					if (defaultIgnoredSignals.contains(req.sig_number()))
						co_await removePendingSignal(req.sig_number());
				}else if(req.sig_handler() == uintptr_t(SIG_IGN)) {
					// POSIX requires discarding pending signals when setting SIG_IGN
					handler.disposition = SignalDisposition::ignore;
					co_await removePendingSignal(req.sig_number());
				}else{
					handler.disposition = SignalDisposition::handle;
					handler.handlerIp = req.sig_handler();
				}

				handler.flags = 0;
				handler.mask = req.sig_mask();
				handler.restorerIp = req.sig_restorer();

				if(req.flags() & SA_SIGINFO)
					handler.flags |= signalInfo;
				if(req.flags() & SA_RESETHAND)
					handler.flags |= signalOnce;
				if(req.flags() & SA_NODEFER)
					handler.flags |= signalReentrant;
				if(req.flags() & SA_ONSTACK)
					handler.flags |= signalOnStack;
				if(req.flags() & SA_NOCLDSTOP)
					std::cout << "\e[31mposix: Ignoring SA_NOCLDSTOP\e[39m" << std::endl;
				if(req.flags() & SA_NOCLDWAIT)
					handler.flags |= signalNoChildWait;

				saved_handler = self->threadGroup()->signalContext()->changeHandler(req.sig_number(), handler);
			}else{
				saved_handler = self->threadGroup()->signalContext()->getHandler(req.sig_number());
			}

			int saved_flags = 0;
			if(saved_handler.flags & signalInfo)
				saved_flags |= SA_SIGINFO;
			if(saved_handler.flags & signalOnce)
				saved_flags |= SA_RESETHAND;
			if(saved_handler.flags & signalReentrant)
				saved_flags |= SA_NODEFER;
			if(saved_handler.flags & signalOnStack)
				saved_flags |= SA_ONSTACK;
			if(saved_handler.flags & signalNoChildWait)
				saved_flags |= SA_NOCLDWAIT;

			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_flags(saved_flags);
			resp.set_sig_mask(saved_handler.mask);
			if(saved_handler.disposition == SignalDisposition::handle) {
				resp.set_sig_handler(saved_handler.handlerIp);
				resp.set_sig_restorer(saved_handler.restorerIp);
			}else if(saved_handler.disposition == SignalDisposition::none) {
				resp.set_sig_handler((uint64_t)SIG_DFL);
			}else{
				assert(saved_handler.disposition == SignalDisposition::ignore);
				resp.set_sig_handler((uint64_t)SIG_IGN);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::PIPE_CREATE) {
			logRequest(logRequests, "PIPE_CREATE");

			assert(!(req.flags() & ~(O_CLOEXEC | O_NONBLOCK)));

			bool nonBlock = false;

			if(req.flags() & O_NONBLOCK)
				nonBlock = true;

			auto pair = fifo::createPair(nonBlock);
			auto r_fd = self->fileContext()->attachFile(std::get<0>(pair),
					req.flags() & O_CLOEXEC);
			auto w_fd = self->fileContext()->attachFile(std::get<1>(pair),
					req.flags() & O_CLOEXEC);

			managarm::posix::SvrResponse resp;
			if (r_fd && w_fd) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.add_fds(r_fd.value());
				resp.add_fds(w_fd.value());
			} else {
				resp.set_error((!r_fd ? r_fd.error() : w_fd.error()) | toPosixProtoError);
				if (r_fd)
					self->fileContext()->closeFile(r_fd.value());
				if (w_fd)
					self->fileContext()->closeFile(w_fd.value());
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::SETSID) {
			logRequest(logRequests, "SETSID");

			managarm::posix::SvrResponse resp;

			// POSIX: if the calling process is already a group leader, EPERM.
			if(self->pgPointer()->getSession()->getSessionId() == self->pid()) {
				co_await sendErrorResponse(managarm::posix::Errors::INSUFFICIENT_PERMISSION);
				continue;
			}

			auto session = TerminalSession::initializeNewSession(self->threadGroup());

			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_sid(session->getSessionId());

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_CALL) {
			logRequest(logRequests, "EPOLL_CALL");

			// Since file descriptors may appear multiple times in a poll() call,
			// we need to de-duplicate them here.
			std::unordered_map<int, unsigned int> fdsToEvents;

			auto epfile = epoll::createFile();
			assert(req.fds_size() == req.events_size());

			auto timeout = req.timeout();
			bool errorOut = false;
			size_t epollAddedItems = 0;

			for(size_t i = 0; i < req.fds_size(); i++) {
				auto [mapIt, inserted] = fdsToEvents.insert({req.fds(i), 0});
				if(!inserted)
					continue;

				// if fd is < 0, `events` shall be ignored and revents set to 0
				if (req.fds(i) < 0)
					continue;

				auto file = self->fileContext()->getFile(req.fds(i));
				if(!file) {
					// poll() is supposed to fail on a per-FD basis.
					mapIt->second = POLLNVAL;
					timeout = 0;
					continue;
				}
				auto locked = file->weakFile().lock();
				assert(locked);

				// Translate POLL events to EPOLL events.
				if(req.events(i) & ~(POLLIN | POLLPRI | POLLOUT | POLLRDHUP | POLLERR | POLLHUP
						| POLLNVAL | POLLWRNORM | POLLRDNORM)) {
					std::cout << "\e[31mposix: Unexpected events for poll()\e[39m" << std::endl;
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					errorOut = true;
					break;
				}

				unsigned int mask = 0;
				if(req.events(i) & POLLIN) mask |= EPOLLIN;
				if(req.events(i) & POLLRDNORM) mask |= EPOLLIN;
				if(req.events(i) & POLLOUT) mask |= EPOLLOUT;
				if(req.events(i) & POLLWRNORM) mask |= EPOLLOUT;
				if(req.events(i) & POLLPRI) mask |= EPOLLPRI;
				if(req.events(i) & POLLRDHUP) mask |= EPOLLRDHUP;
				if(req.events(i) & POLLERR) mask |= EPOLLERR;
				if(req.events(i) & POLLHUP) mask |= EPOLLHUP;

				// addItem() can fail with EEXIST but we check for duplicate FDs above
				// so that cannot happen here.
				Error ret = epoll::addItem(epfile.get(), self.get(),
					std::move(locked), req.fds(i), mask, req.fds(i));
				assert(ret == Error::success);
				epollAddedItems++;
			}
			if(errorOut)
				continue;

			struct epoll_event events[16] = {};
			size_t k = 0;
			bool interrupted = false;

			if (epollAddedItems) {
				auto cancelEvent = self->cancelEventRegistry().event(self->credentials(), req.cancellation_id());
				if (!cancelEvent) {
					std::println("posix: possibly duplicate cancellation ID registered");
					sendErrorResponse(managarm::posix::Errors::INTERNAL_ERROR);
					continue;
				}

				if(timeout < 0) {
					co_await async::race_and_cancel(
						async::lambda([&](auto c) -> async::result<void> {
							co_await async::suspend_indefinitely(c, cancelEvent);
							// if the cancelEvent was raised, we consider this wait to have been
							// interrupted.
							if (async::cancellation_token{cancelEvent}.is_cancellation_requested())
								interrupted = true;
						}),
						async::lambda([&](auto c) -> async::result<void> {
							if (req.has_signal_seq() && self->enteredSignalSeq() != req.signal_seq()) {
								// a signal was already raised since the request's
								// signal seqnum
								interrupted = true;
								co_return;
							}
							co_await async::suspend_indefinitely(c);
						}),
						async::lambda([&](auto c) -> async::result<void> {
							k = co_await epoll::wait(epfile.get(), events, 16, c);
						})
					);
				}else if(!timeout) {
					// Do not bother to set up a timer for zero timeouts.
					async::cancellation_event cancel_wait;
					cancel_wait.cancel();
					k = co_await epoll::wait(epfile.get(), events, 16, cancel_wait);
				}else{
					assert(timeout > 0);
					co_await async::race_and_cancel(
						async::lambda([&](auto c) -> async::result<void> {
							// if the timeout runs to completion, i.e. the sleep does not return
							// false to signal cancellation, we DO NOT consider the call to have
							// been interrupted.
							co_await helix::sleepFor(static_cast<uint64_t>(timeout), c);
						}),
						async::lambda([&](auto c) -> async::result<void> {
							co_await async::suspend_indefinitely(c, cancelEvent);
							// if the cancelEvent was raised, we consider this wait to have been
							// interrupted.
							if (async::cancellation_token{cancelEvent}.is_cancellation_requested())
								interrupted = true;
						}),
						async::lambda([&](auto c) -> async::result<void> {
							if (req.has_signal_seq() && self->enteredSignalSeq() != req.signal_seq()) {
								// a signal was already raised since the request's
								// signal seqnum
								interrupted = true;
								co_return;
							}
							co_await async::suspend_indefinitely(c);
						}),
						async::lambda([&](auto c) -> async::result<void> {
							k = co_await epoll::wait(epfile.get(), events, 16, c);
						})
					);
				}
			}

			// Assigned the returned events to each FD.
			for(size_t j = 0; j < k; ++j) {
				auto it = fdsToEvents.find(events[j].data.fd);
				assert(it != fdsToEvents.end());

				// Translate EPOLL events back to POLL events.
				assert(!it->second);
				if(events[j].events & EPOLLIN) it->second |= POLLIN;
				if(events[j].events & EPOLLOUT) it->second |= POLLOUT;
				if(events[j].events & EPOLLPRI) it->second |= POLLPRI;
				if(events[j].events & EPOLLRDHUP) it->second |= POLLRDHUP;
				if(events[j].events & EPOLLERR) it->second |= POLLERR;
				if(events[j].events & EPOLLHUP) it->second |= POLLHUP;
			}

			managarm::posix::SvrResponse resp;
			bool hasEvents = false;

			for(size_t i = 0; i < req.fds_size(); ++i) {
				auto it = fdsToEvents.find(req.fds(i));
				assert(it != fdsToEvents.end());
				resp.add_events(it->second);
				if (!hasEvents && it->second)
					hasEvents = true;
			}

			if (!hasEvents && interrupted) {
				resp.set_error(managarm::posix::Errors::INTERRUPTED);
			} else {
				resp.set_error(managarm::posix::Errors::SUCCESS);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_CREATE) {
			logRequest(logRequests, "EPOLL_CREATE");

			assert(!(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC)));

			auto file = epoll::createFile();
			auto fd = self->fileContext()->attachFile(file,
					req.flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

			managarm::posix::SvrResponse resp;
			if (fd) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd.value());
			} else {
				resp.set_error(fd.error() | toPosixProtoError);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_ADD) {
			logRequest(logRequests, "EPOLL_ADD", "epollfd={} fd={}", req.fd(), req.newfd());

			auto epfile = self->fileContext()->getFile(req.fd());
			auto file = self->fileContext()->getFile(req.newfd());
			if(!file || !epfile) {
				co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
				continue;
			}

			auto locked = file->weakFile().lock();
			assert(locked);
			Error ret = epoll::addItem(epfile.get(), self.get(),
					std::move(locked), req.newfd(), req.flags(), req.cookie());
			if(ret == Error::alreadyExists) {
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}
			assert(ret == Error::success);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_MODIFY) {
			logRequest(logRequests, "EPOLL_MODIFY");

			auto epfile = self->fileContext()->getFile(req.fd());
			auto file = self->fileContext()->getFile(req.newfd());
			assert(epfile && "Illegal FD for EPOLL_MODIFY");
			assert(file && "Illegal FD for EPOLL_MODIFY item");

			Error ret = epoll::modifyItem(epfile.get(), file.get(), req.newfd(),
					req.flags(), req.cookie());
			if(ret == Error::noSuchFile) {
				co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
				continue;
			}
			assert(ret == Error::success);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_DELETE) {
			logRequest(logRequests, "EPOLL_DELETE");

			auto epfile = self->fileContext()->getFile(req.fd());
			auto file = self->fileContext()->getFile(req.newfd());
			if(!epfile || !file) {
				std::cout << "posix: Illegal FD for EPOLL_DELETE" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
				continue;
			}

			Error ret = epoll::deleteItem(epfile.get(), file.get(), req.newfd(), req.flags());
			if(ret == Error::noSuchFile) {
				co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
				continue;
			}
			assert(ret == Error::success);

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_WAIT) {
			logRequest(logRequests, "EPOLL_WAIT", "epollfd={}", req.fd());

			uint64_t former = self->signalMask();

			auto epfile = self->fileContext()->getFile(req.fd());
			if(!epfile) {
				co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
				continue;
			}
			if(req.sigmask_needed()) {
				self->setSignalMask(req.sigmask());
			}

			struct epoll_event events[16];
			size_t k;
			if(req.timeout() < 0) {
				k = co_await epoll::wait(epfile.get(), events,
						std::min(req.size(), uint32_t(16)));
			}else if(!req.timeout()) {
				// Do not bother to set up a timer for zero timeouts.
				async::cancellation_event cancel_wait;
				cancel_wait.cancel();
				k = co_await epoll::wait(epfile.get(), events,
						std::min(req.size(), uint32_t(16)), cancel_wait);
			}else{
				assert(req.timeout() > 0);
				async::cancellation_event cancel_wait;
				helix::TimeoutCancellation timer{static_cast<uint64_t>(req.timeout()), cancel_wait};
				k = co_await epoll::wait(epfile.get(), events, 16, cancel_wait);
				co_await timer.retire();
			}
			if(req.sigmask_needed()) {
				self->setSignalMask(former);
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(events, k * sizeof(struct epoll_event))
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::SIGNALFD_CREATE) {
			logRequest(logRequests, "SIGNALFD_CREATE");

			if(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC
					| managarm::posix::OpenFlags::OF_NONBLOCK)) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			if(req.fd() == -1) {
				auto file = createSignalFile(req.sigset(),
						req.flags() & managarm::posix::OpenFlags::OF_NONBLOCK);
				auto fd = self->fileContext()->attachFile(file,
						req.flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

				if (fd)
					resp.set_fd(fd.value());
				else
					resp.set_error(fd.error() | toPosixProtoError);
			} else {
				auto file = self->fileContext()->getFile(req.fd());
				if(file) {
					auto signal_file = static_cast<signal_fd::OpenFile *>(file.get());
					signal_file->mask() = req.sigset();
					resp.set_fd(req.fd());
				} else {
					resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);
				}
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else{
			std::cout << "posix: Illegal request" << std::endl;

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
		}

		if(posix::ostContext.isActive()) {
			posix::ostContext.emit(
				posix::ostEvtLegacyRequest,
				posix::ostAttrRequest(req.request_type()),
				posix::ostAttrTime(timer.elapsed())
			);
		}
	}

	if(logCleanup)
		std::cout << "\e[33mposix: Exiting serveRequests()\e[39m" << std::endl;
	generation->requestsDone.raise();
}
