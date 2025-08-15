#include <format>
#include <print>
#include <linux/netlink.h>
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/pidfd.h>
#include <unistd.h>
#include <print>

#include <helix/timer.hpp>

#include "net.hpp"
#include "netlink/nl-socket.hpp"
#include "epoll.hpp"
#include "extern_socket.hpp"
#include "fifo.hpp"
#include "inotify.hpp"
#include "memfd.hpp"
#include "ostrace.hpp"
#include "pts.hpp"
#include "requests.hpp"
#include "signalfd.hpp"
#include "sysfs.hpp"
#include "un-socket.hpp"
#include "timerfd.hpp"
#include "eventfd.hpp"
#include "signalfd.hpp"
#include "tmp_fs.hpp"
#include "cgroupfs.hpp"
#include "pidfd.hpp"

#include <bragi/helpers-std.hpp>
#include <core/clock.hpp>
#include <posix.bragi.hpp>
#include <kerncfg.bragi.hpp>
#include <hw.bragi.hpp>

#include "clocks.hpp"
#include "debug-options.hpp"

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
				posix::ostAttrPid(self->pid()),
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
				posix::ostAttrPid(self->pid()),
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

		if(preamble.id() == bragi::message_id<managarm::posix::GetPidRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::GetPidRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "GET_PID", "pid={}", self->pid());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(self->pid());

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::GetPpidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetPpidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "GET_PPID", "ppid={}", self->getParent()->pid());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(self->getParent()->pid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::GetUidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetUidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "GET_UID", "uid={}", self->uid());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_uid(self->uid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::SetUidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetUidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "SET_UID", "uid={}", req->uid());

			Error err = self->setUid(req->uid());
			if(err == Error::accessDenied) {
				co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
			} else if(err == Error::illegalArguments) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {
				co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
			}
		}else if(preamble.id() == managarm::posix::GetEuidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetEuidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "GET_EUID", "euid={}", self->euid());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_uid(self->euid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::SetEuidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetEuidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "SET_EUID", "euid={}", req->uid());

			Error err = self->setEuid(req->uid());
			if(err == Error::accessDenied) {
				co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
			} else if(err == Error::illegalArguments) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {
				co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
			}
		}else if(preamble.id() == managarm::posix::GetGidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetGidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "GET_GID", "gid={}", self->gid());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_uid(self->gid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::GetEgidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetEgidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "GET_EGID", "egid={}", self->egid());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_uid(self->egid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::SetGidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetGidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "SET_GID");

			Error err = self->setGid(req->uid());
			if(err == Error::accessDenied) {
				co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
			} else if(err == Error::illegalArguments) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {
				co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
			}
		}else if(preamble.id() == managarm::posix::SetEgidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetEgidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "SET_EGID");

			Error err = self->setEgid(req->uid());
			if(err == Error::accessDenied) {
				co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
			} else if(err == Error::illegalArguments) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {
				co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
			}
		}else if(preamble.id() == managarm::posix::WaitIdRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::WaitIdRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if((req->flags() & ~(WNOHANG | WCONTINUED | WEXITED | WSTOPPED | WNOWAIT)) ||
				!(req->flags() & (WEXITED /*| WSTOPPED | WCONTINUED*/))) {
				std::cout << "posix: WAIT_ID invalid flags: " << req->flags() << std::endl;
				co_await sendErrorResponse.template operator()<managarm::posix::WaitIdResponse>
					(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			WaitFlags flags = 0;

			if(req->flags() & WNOHANG)
				flags |= waitNonBlocking;

			if(req->flags() & WEXITED)
				flags |= waitExited;

			if(req->flags() & WSTOPPED)
				std::cout << "\e[31mposix: WAIT_ID flag WSTOPPED is silently ignored\e[39m" << std::endl;

			if(req->flags() & WCONTINUED)
				std::cout << "\e[31mposix: WAIT_ID flag WCONTINUED is silently ignored\e[39m" << std::endl;

			if(req->flags() & WNOWAIT)
				flags |= waitLeaveZombie;

			int wait_pid = 0;
			// TODO (geert): make this operation cancelable.
			if(req->idtype() == P_PID) {
				wait_pid = req->id();
			} else if(req->idtype() == P_ALL) {
				wait_pid = -1;
			} else if(req->idtype() == P_PIDFD) {
				auto fd = self->fileContext()->getFile(req->id());
				if(!fd || fd->kind() != FileKind::pidfd) {
					co_await sendErrorResponse.template operator()<managarm::posix::WaitIdResponse>
						(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}
				auto pidfd = smarter::static_pointer_cast<pidfd::OpenFile>(fd);
				wait_pid = pidfd->pid();
				if(pidfd->nonBlock())
					flags |= waitNonBlocking;
			} else {
				std::cout << "\e[31mposix: WAIT_ID idtype other than P_PID, P_PIDFD and P_ALL are not implemented\e[39m" << std::endl;
				co_await sendErrorResponse.template operator()<managarm::posix::WaitIdResponse>
					(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			logRequest(logRequests, "WAIT_ID", "pid={}", wait_pid);

			auto wait_result = co_await self->wait(wait_pid, flags, {});

			managarm::posix::WaitIdResponse resp;

			if(wait_result) {
				auto proc_state = wait_result.value();
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_pid(proc_state.pid);
				resp.set_uid(proc_state.uid);

				if(auto byExit = std::get_if<TerminationByExit>(&proc_state.state); byExit) {
					resp.set_sig_status(W_EXITCODE(byExit->code, 0));
					resp.set_sig_code(CLD_EXITED);
				}else if(auto bySignal = std::get_if<TerminationBySignal>(&proc_state.state); bySignal) {
					resp.set_sig_status(W_EXITCODE(0, bySignal->signo));
					resp.set_sig_code(self->getDumpable() ? CLD_DUMPED : CLD_KILLED);
				}else{
					resp.set_sig_status(0);
					resp.set_sig_code(0);
					assert(std::holds_alternative<std::monostate>(proc_state.state));
				}
			} else if (wait_result.error() == Error::wouldBlock) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_pid(0);
			} else {
				assert(wait_result.error() == Error::noChildProcesses);
				resp.set_error(managarm::posix::Errors::NO_CHILD_PROCESSES);
			}

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::WAIT) {
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
				assert(waitResult.error() == Error::noChildProcesses);
				resp.set_error(managarm::posix::Errors::NO_CHILD_PROCESSES);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::RebootRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::RebootRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "REBOOT", "command={}", req->cmd());

			if(self->uid() != 0) {
				sendErrorResponse(managarm::posix::Errors::INSUFFICIENT_PERMISSION);
				continue;
			}

			managarm::hw::RebootRequest hwRequest;
			hwRequest.set_cmd(req->cmd());
			auto [offer, hwSendResp, hwResp] = co_await helix_ng::exchangeMsgs(
				getPmLane(),
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(hwRequest, frg::stl_allocator{}),
					helix_ng::recvInline()
				)
			);
			HEL_CHECK(offer.error());
			HEL_CHECK(hwSendResp.error());
			HEL_CHECK(hwResp.error());
			hwResp.reset();

			managarm::posix::SvrResponse resp;

			resp.set_error(managarm::posix::Errors::SUCCESS);

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
				user_time = self->accumulatedUsage().userTime;
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
		}else if(preamble.id() == bragi::message_id<managarm::posix::VmMapRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::VmMapRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "VM_MAP", "size={:#x}", req->size());

			// TODO: Validate req->flags().

			if(req->mode() & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			if(req->rel_offset() & 0xFFF) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			uint32_t nativeFlags = 0;

			if(req->mode() & PROT_READ)
				nativeFlags |= kHelMapProtRead;
			if(req->mode() & PROT_WRITE)
				nativeFlags |= kHelMapProtWrite;
			if(req->mode() & PROT_EXEC)
				nativeFlags |= kHelMapProtExecute;

			if(req->flags() & MAP_FIXED_NOREPLACE)
				nativeFlags |= kHelMapFixedNoReplace;
			else if(req->flags() & MAP_FIXED)
				nativeFlags |= kHelMapFixed;

			bool copyOnWrite;
			if((req->flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_PRIVATE) {
				copyOnWrite = true;
			}else if((req->flags() & (MAP_PRIVATE | MAP_SHARED)) == MAP_SHARED) {
				copyOnWrite = false;
			}else{
				throw std::runtime_error("posix: Handle illegal flags in VM_MAP");
			}

			uintptr_t hint = req->address_hint();

			frg::expected<Error, void *> result;
			if(req->flags() & MAP_ANONYMOUS) {
				assert(!req->rel_offset());

				if(copyOnWrite) {
					result = co_await self->vmContext()->mapFile(hint,
							{}, nullptr,
							0, req->size(), true, nativeFlags);
				}else{
					HelHandle handle;
					HEL_CHECK(helAllocateMemory(req->size(), 0, nullptr, &handle));

					result = co_await self->vmContext()->mapFile(hint,
							helix::UniqueDescriptor{handle}, nullptr,
							0, req->size(), false, nativeFlags);
				}
			}else{
				auto file = self->fileContext()->getFile(req->fd());
				assert(file && "Illegal FD for VM_MAP");
				auto memory = co_await file->accessMemory();
				assert(memory);
				result = co_await self->vmContext()->mapFile(hint,
						std::move(memory), std::move(file),
						req->rel_offset(), req->size(), copyOnWrite, nativeFlags);
			}

			if(!result) {
				assert(result.error() == Error::alreadyExists || result.error() == Error::noMemory);
				if(result.error() == Error::alreadyExists)
					co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				else if(result.error() == Error::noMemory)
					co_await sendErrorResponse(managarm::posix::Errors::NO_MEMORY);
				continue;
			}

			void *address = result.unwrap();

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_offset(reinterpret_cast<uintptr_t>(address));

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
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

			self->vmContext()->unmapFile(reinterpret_cast<void *>(req.address()), req.size());

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::MountRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::MountRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "MOUNT", "fstype={} on={} to={}", req->fs_type(), req->path(), req->target_path());

			auto resolveResult = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req->target_path(), self.get());
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}
			auto target = resolveResult.value();

			if(req->fs_type() == "procfs" || req->fs_type() == "proc") {
				co_await target.first->mount(target.second, getProcfs());
			}else if(req->fs_type() == "sysfs") {
				co_await target.first->mount(target.second, getSysfs());
			}else if(req->fs_type() == "devtmpfs") {
				co_await target.first->mount(target.second, getDevtmpfs());
			}else if(req->fs_type() == "tmpfs") {
				co_await target.first->mount(target.second, tmp_fs::createRoot());
			}else if(req->fs_type() == "devpts") {
				co_await target.first->mount(target.second, pts::getFsRoot());
			}else if(req->fs_type() == "cgroup2") {
				co_await target.first->mount(target.second, getCgroupfs());
			}else{
				if(req->fs_type() != "ext2") {
					std::cout << "posix: Trying to mount unsupported FS of type: " << req->fs_type() << std::endl;
				}
				assert(req->fs_type() == "ext2");
				auto sourceResult = co_await resolve(self->fsContext()->getRoot(),
						self->fsContext()->getWorkingDirectory(), req->path(), self.get());
				if(!sourceResult) {
					if(sourceResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(sourceResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}
				auto source = sourceResult.value();
				assert(source.second);
				assert(source.second->getTarget()->getType() == VfsType::blockDevice);
				auto device = blockRegistry.get(source.second->getTarget()->readDevice());
				auto link = co_await device->mount();
				co_await target.first->mount(target.second, std::move(link), source);
			}

			logRequest(logRequests, "MOUNT", "succeeded");

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
					);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::ChrootRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::ChrootRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "CHROOT");

			auto pathResult = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req->path(), self.get());
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse.template operator()<managarm::posix::ChrootResponse>
						(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse.template operator()<managarm::posix::ChrootResponse>
						(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}
			auto path = pathResult.value();
			self->fsContext()->changeRoot(path);

			managarm::posix::ChrootResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::ChdirRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::ChdirRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "CHDIR");

			auto pathResult = co_await resolve(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req->path(), self.get());
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse.template operator()<managarm::posix::ChdirResponse>
						(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse.template operator()<managarm::posix::ChdirResponse>
						(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}
			auto path = pathResult.value();
			self->fsContext()->changeWorkingDirectory(path);

			managarm::posix::ChdirResponse resp;
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
		}else if(preamble.id() == managarm::posix::AccessAtRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::AccessAtRequest>(recv_head, tail);

			if(!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			ResolveFlags resolveFlags = {};

			if(req->flags() & AT_SYMLINK_NOFOLLOW)
				resolveFlags |= resolveDontFollow;

			if(req->flags() & ~(AT_SYMLINK_NOFOLLOW)) {
				if(req->flags() & AT_EACCESS) {
					std::cout << "posix: ACCESSAT flag handling AT_EACCESS is unimplemented" << std::endl;
				} else {
					std::cout << "posix: ACCESSAT unknown flag is unimplemented: " << req->flags() << std::endl;
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
			}

			if(req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if(!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			auto pathResult = co_await resolve(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get(), resolveFlags);
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			logRequest(logRequests || logPaths, "ACCESSAT", "'{}'", pathResult.value().getPath(self->fsContext()->getRoot()));

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::MkdirAtRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::MkdirAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests || logPaths, "MKDIRAT", "path='{}'", req->path());

			if(!req->path().size()) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			if(req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve(resolvePrefix);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			if(!resolver.hasComponent()) {
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}

			auto parent = resolver.currentLink()->getTarget();
			auto existsResult = co_await parent->getLink(resolver.nextComponent());
			if (existsResult) {
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}

			auto result = co_await parent->mkdir(resolver.nextComponent());

			if(auto error = std::get_if<Error>(&result); error) {
				assert(*error == Error::illegalOperationTarget);
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			auto target = std::get<std::shared_ptr<FsLink>>(result)->getTarget();
			auto chmodResult = co_await target->chmod(req->mode() & ~self->fsContext()->getUmask() & 0777);
			if (chmodResult != Error::success) {
				std::cout << "posix: chmod failed when creating directory for MkdirAtRequest!" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::INTERNAL_ERROR);
				continue;
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::MkfifoAtRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::MkfifoAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests || logPaths, "MKFIFOAT", "path='{}'", req->path());

			if (!req->path().size()) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsLink> target_link;

			if (req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve(resolvePrefix | resolveNoTrailingSlash);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			auto parent = resolver.currentLink()->getTarget();
			if(co_await parent->getLink(resolver.nextComponent())) {
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}

			auto result = co_await parent->mkfifo(
				resolver.nextComponent(),
				req->mode() & ~self->fsContext()->getUmask()
			);
			if(!result) {
				std::cout << "posix: Unexpected failure from mkfifo()" << std::endl;
				co_return;
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::LinkAtRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::LinkAtRequest>(recv_head, tail);

			logRequest(logRequests, "LINKAT");

			if(req->flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW)) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			if(req->flags() & AT_EMPTY_PATH) {
				std::cout << "posix: AT_EMPTY_PATH is unimplemented for linkat" << std::endl;
			}

			if(req->flags() & AT_SYMLINK_FOLLOW) {
				std::cout << "posix: AT_SYMLINK_FOLLOW is unimplemented for linkat" << std::endl;
			}

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			if(req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if(!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			if (req->newfd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->newfd());

				if(!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver new_resolver;
			new_resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->target_path(), self.get());
			auto new_resolveResult = co_await new_resolver.resolve(
					resolvePrefix | resolveNoTrailingSlash);
			if(!new_resolveResult) {
				if(new_resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
					continue;
				} else if(new_resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(new_resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			auto target = resolver.currentLink()->getTarget();
			auto directory = new_resolver.currentLink()->getTarget();
			assert(target->superblock() == directory->superblock()); // Hard links across mount points are not allowed, return EXDEV
			auto result = co_await directory->link(new_resolver.nextComponent(), target);
			if(!result) {
				std::cout << "posix: Unexpected failure from link()" << std::endl;
				co_return;
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::SymlinkAtRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::SymlinkAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			ViewPath relativeTo;
			smarter::shared_ptr<File, FileHandle> file;

			if (!req->path().size()) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			if(req->fd() == AT_FDCWD) {
				relativeTo = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());
				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relativeTo = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relativeTo, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve(
					resolvePrefix | resolveNoTrailingSlash);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			logRequest(logRequests || logPaths, "SYMLINK", "'{}{}' -> '{}'",
				ViewPath{resolver.currentView(), resolver.currentLink()}
					.getPath(self->fsContext()->getRoot()),
				resolver.nextComponent(),
				req->target_path());

			auto parent = resolver.currentLink()->getTarget();
			auto result = co_await parent->symlink(resolver.nextComponent(), req->target_path());
			if(auto error = std::get_if<Error>(&result); error) {
				if(*error == Error::alreadyExists) {
					co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
					continue;
				} else {
					assert(*error == Error::illegalOperationTarget);
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::RenameAtRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::RenameAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			if (req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve(resolveDontFollow);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::isDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::IS_DIRECTORY);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			if (req->newfd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->newfd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			// TODO: Add resolveNoTrailingSlash if source is not a directory?
			PathResolver new_resolver;
			new_resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->target_path(), self.get());
			auto new_resolveResult = co_await new_resolver.resolve(resolvePrefix);
			if(!new_resolveResult) {
				if(new_resolveResult.error() == protocols::fs::Error::isDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::IS_DIRECTORY);
					continue;
				} else if(new_resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(new_resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			logRequest(logRequests || logPaths, "RENAMEAT", "'{}' -> '{}{}'",
				ViewPath(resolver.currentView(), resolver.currentLink())
				.getPath(self->fsContext()->getRoot()),
				ViewPath(new_resolver.currentView(), new_resolver.currentLink())
				.getPath(self->fsContext()->getRoot()),
				new_resolver.nextComponent());

			auto superblock = resolver.currentLink()->getTarget()->superblock();
			auto directory = new_resolver.currentLink()->getTarget();
			assert(superblock == directory->superblock());
			auto result = co_await superblock->rename(resolver.currentLink().get(),
					directory.get(), new_resolver.nextComponent());
			if(!result) {
				assert(result.error() == Error::alreadyExists);
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::FstatAtRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::FstatAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "FSTATAT");

			if (req->flags() & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT)) {
				std::cout << std::format("posix: unsupported flags {:#x} given to FSTATAT request", req->flags()) << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsLink> target_link;
			std::shared_ptr<MountView> target_mount;

			if (req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			if (req->flags() & AT_EMPTY_PATH) {
				target_link = file->associatedLink();
			} else {
				PathResolver resolver;
				resolver.setup(self->fsContext()->getRoot(),
						relative_to, req->path(), self.get());

				ResolveFlags resolveFlags = 0;
				if (req->flags() & AT_SYMLINK_NOFOLLOW)
				    resolveFlags |= resolveDontFollow;

				auto resolveResult = co_await resolver.resolve(resolveFlags);
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}

				target_mount = resolver.currentView();
				target_link = resolver.currentLink();
			}

			// This catches cases where associatedLink is called on a file, but the file doesn't implement that.
			// Instead of blowing up, return ENOENT.
			if(target_link == nullptr) {
				co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
				continue;
			}

			auto statsResult = co_await target_link->getTarget()->getStats();
			managarm::posix::SvrResponse resp;

			if (statsResult) {
				auto stats = statsResult.value();

				resp.set_error(managarm::posix::Errors::SUCCESS);

				DeviceId devnum;
				switch(target_link->getTarget()->getType()) {
				case VfsType::regular:
					resp.set_file_type(managarm::posix::FileType::FT_REGULAR);
					break;
				case VfsType::directory:
					resp.set_file_type(managarm::posix::FileType::FT_DIRECTORY);
					break;
				case VfsType::symlink:
					resp.set_file_type(managarm::posix::FileType::FT_SYMLINK);
					break;
				case VfsType::charDevice:
					resp.set_file_type(managarm::posix::FileType::FT_CHAR_DEVICE);
					devnum = target_link->getTarget()->readDevice();
					resp.set_ref_devnum(makedev(devnum.first, devnum.second));
					break;
				case VfsType::blockDevice:
					resp.set_file_type(managarm::posix::FileType::FT_BLOCK_DEVICE);
					devnum = target_link->getTarget()->readDevice();
					resp.set_ref_devnum(makedev(devnum.first, devnum.second));
					break;
				case VfsType::socket:
					resp.set_file_type(managarm::posix::FileType::FT_SOCKET);
					break;
				case VfsType::fifo:
					resp.set_file_type(managarm::posix::FileType::FT_FIFO);
					break;
				default:
					assert(target_link->getTarget()->getType() == VfsType::null);
				}

				if(stats.mode & ~0xFFFu)
					std::cout << "\e[31m" "posix: FsNode::getStats() returned illegal mode of "
							<< stats.mode << "\e[39m" << std::endl;

				resp.set_fs_inode(stats.inodeNumber);
				resp.set_mode(stats.mode);
				resp.set_num_links(stats.numLinks);
				resp.set_uid(stats.uid);
				resp.set_gid(stats.gid);
				resp.set_file_size(stats.fileSize);
				resp.set_atime_secs(stats.atimeSecs);
				resp.set_atime_nanos(stats.atimeNanos);
				resp.set_mtime_secs(stats.mtimeSecs);
				resp.set_mtime_nanos(stats.mtimeNanos);
				resp.set_ctime_secs(stats.ctimeSecs);
				resp.set_ctime_nanos(stats.ctimeNanos);
				resp.set_mount_id(target_mount ? target_mount->mountId() : 0);
				resp.set_stat_dev(target_link->getTarget()->superblock()->deviceNumber());
			} else {
				resp.set_error(statsResult.error() | toPosixProtoError);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::FstatfsRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recvTail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
			);
			HEL_CHECK(recvTail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::FstatfsRequest>(recv_head, tail);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "FSTATFS");

			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsLink> target_link;
			managarm::posix::FstatfsResponse resp;

			if(req->fd() >= 0) {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse.template operator()<managarm::posix::FstatfsResponse>
						(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				target_link = file->associatedLink();

				// This catches cases where associatedLink is called on a file, but the file doesn't implement that.
				// Instead of blowing up, return ENOENT.
				// TODO: fstatfs can't return ENOENT, verify this is needed
				if(target_link == nullptr) {
					co_await sendErrorResponse.template operator()<managarm::posix::FstatfsResponse>
						(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				}

				auto fsstatsResult = co_await target_link->getTarget()->superblock()->getFsstats();
				if(!fsstatsResult) {
					co_await sendErrorResponse.template operator()<managarm::posix::FstatfsResponse>
						(fsstatsResult.error() | toPosixProtoError);
					continue;
				}
				auto fsstats = fsstatsResult.value();

				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fstype(fsstats.f_type);
			} else {
				PathResolver resolver;
				resolver.setup(self->fsContext()->getRoot(), self->fsContext()->getWorkingDirectory(),
						req->path(), self.get());
				auto resolveResult = co_await resolver.resolve();
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse.template operator()<managarm::posix::FstatfsResponse>
							(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse.template operator()<managarm::posix::FstatfsResponse>
							(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}

				target_link = resolver.currentLink();
				auto fsstatsResult = co_await target_link->getTarget()->superblock()->getFsstats();
				if(!fsstatsResult) {
					co_await sendErrorResponse.template operator()<managarm::posix::FstatfsResponse>
						(fsstatsResult.error() | toPosixProtoError);
					continue;
				}
				auto fsstats = fsstatsResult.value();

				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fstype(fsstats.f_type);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::FchmodAtRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::FchmodAtRequest>(recv_head, tail);

			logRequest(logRequests, "FCHMODAT");

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsLink> target_link;

			if(req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			if(req->flags()) {
				if(req->flags() & AT_SYMLINK_NOFOLLOW) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_SUPPORTED);
					continue;
				} else if(req->flags() & AT_EMPTY_PATH) {
					// Allowed, managarm extension
				} else {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
			}

			if(req->flags() & AT_EMPTY_PATH) {
				target_link = file->associatedLink();
			} else {
				PathResolver resolver;
				resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get());

				auto resolveResult = co_await resolver.resolve();

				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}

				target_link = resolver.currentLink();
			}

			co_await target_link->getTarget()->chmod(req->mode());

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::UtimensAtRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::UtimensAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests || logPaths, "UTIMENSAT");

			ViewPath relativeTo;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsNode> target = nullptr;

			if(!req->path().size() && (req->flags() & AT_EMPTY_PATH)) {
				target = self->fileContext()->getFile(req->fd())->associatedLink()->getTarget();
			} else {
				if(req->flags() & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}

				ResolveFlags resolveFlags = 0;
				if(req->flags() & AT_SYMLINK_NOFOLLOW) {
					resolveFlags |= resolveDontFollow;
				}

				if(req->fd() == AT_FDCWD) {
					relativeTo = self->fsContext()->getWorkingDirectory();
				} else {
					file = self->fileContext()->getFile(req->fd());
					if (!file) {
						co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
						continue;
					}

					relativeTo = {file->associatedMount(), file->associatedLink()};
				}

				PathResolver resolver;
				resolver.setup(self->fsContext()->getRoot(),
						relativeTo, req->path(), self.get());
				auto resolveResult = co_await resolver.resolve(resolveFlags);
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}

				target = resolver.currentLink()->getTarget();
			}

			std::optional<timespec> atime = std::nullopt;
			std::optional<timespec> mtime = std::nullopt;

			auto time = clk::getRealtime();
			if(req->atimeNsec() == UTIME_NOW) {
				atime = {time.tv_sec, time.tv_nsec};
			} else if(req->atimeNsec() != UTIME_OMIT) {
				if(req->atimeNsec() > 999'999'999) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
				atime = {static_cast<time_t>(req->atimeSec()), static_cast<long>(req->atimeNsec())};
			}

			if(req->mtimeNsec() == UTIME_NOW) {
				mtime = {time.tv_sec, time.tv_nsec};
			} else if(req->mtimeNsec() != UTIME_OMIT) {
				if(req->mtimeNsec() > 999'999'999) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
				mtime = {static_cast<time_t>(req->mtimeSec()), static_cast<long>(req->mtimeNsec())};
			}

			co_await target->utimensat(atime, mtime, time);

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == bragi::message_id<managarm::posix::ReadlinkAtRequest>) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::ReadlinkAtRequest>(recv_head, tail);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			if(req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			auto pathResult = co_await resolve(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get(), resolveDontFollow);
			if(!pathResult) {
				if(pathResult.error() == protocols::fs::Error::fileNotFound) {
					managarm::posix::SvrResponse resp;
					resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

					auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
						helix_ng::sendBuffer(nullptr, 0)
					);
					HEL_CHECK(send_resp.error());
					logBragiReply(resp);
					continue;
				} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
					managarm::posix::SvrResponse resp;
					resp.set_error(managarm::posix::Errors::NOT_A_DIRECTORY);

					auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
						helix_ng::sendBuffer(nullptr, 0)
					);
					HEL_CHECK(send_resp.error());
					logBragiReply(resp);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}
			auto path = pathResult.value();

			auto result = co_await path.second->getTarget()->readSymlink(path.second.get(), self.get());
			if(auto error = std::get_if<Error>(&result); error) {
				assert(*error == Error::illegalOperationTarget);

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

				auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
					helix_ng::sendBuffer(nullptr, 0)
				);
				HEL_CHECK(send_resp.error());
				logBragiReply(resp);
			}else{
				auto &target = std::get<std::string>(result);

				logRequest(logRequests || logPaths, "READLINKAT", "'{}' -> '{}'",
					path.getPath(self->fsContext()->getRoot()),
					target);

				managarm::posix::SvrResponse resp;
				resp.set_error(managarm::posix::Errors::SUCCESS);

				auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
					helix_ng::sendBuffer(target.data(), target.size())
				);
				HEL_CHECK(send_resp.error());
				logBragiReply(resp);
			}
		}else if(preamble.id() == bragi::message_id<managarm::posix::OpenAtRequest>) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::OpenAtRequest>(recv_head, tail);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			if((req->flags() & ~(managarm::posix::OpenFlags::OF_CREATE
					| managarm::posix::OpenFlags::OF_EXCLUSIVE
					| managarm::posix::OpenFlags::OF_NONBLOCK
					| managarm::posix::OpenFlags::OF_CLOEXEC
					| managarm::posix::OpenFlags::OF_TRUNC
					| managarm::posix::OpenFlags::OF_RDONLY
					| managarm::posix::OpenFlags::OF_WRONLY
					| managarm::posix::OpenFlags::OF_RDWR
					| managarm::posix::OpenFlags::OF_PATH
					| managarm::posix::OpenFlags::OF_NOCTTY
					| managarm::posix::OpenFlags::OF_APPEND
					| managarm::posix::OpenFlags::OF_NOFOLLOW
					| managarm::posix::OpenFlags::OF_DIRECTORY))) {
				std::cout << "posix: OPENAT flags not recognized: " << req->flags() << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			if (req->path().length() > PATH_MAX) {
				co_await sendErrorResponse(managarm::posix::Errors::NAME_TOO_LONG);
				continue;
			}

			SemanticFlags semantic_flags = 0;
			if(req->flags() & managarm::posix::OpenFlags::OF_NONBLOCK)
				semantic_flags |= semanticNonBlock;

			if (req->flags() & managarm::posix::OpenFlags::OF_RDONLY)
				semantic_flags |= semanticRead;
			else if (req->flags() & managarm::posix::OpenFlags::OF_WRONLY)
				semantic_flags |= semanticWrite;
			else if (req->flags() & managarm::posix::OpenFlags::OF_RDWR)
				semantic_flags |= semanticRead | semanticWrite;

			if(req->flags() & managarm::posix::OpenFlags::OF_APPEND)
				semantic_flags |= semanticAppend;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsLink> target_link;

			if(req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get());
			if(req->flags() & managarm::posix::OpenFlags::OF_CREATE) {
				auto resolveResult = co_await resolver.resolve(
						resolvePrefix | resolveNoTrailingSlash);
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::isDirectory) {
						// TODO: Verify additional constraints for sending EISDIR.
						co_await sendErrorResponse(managarm::posix::Errors::IS_DIRECTORY);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::nameTooLong) {
						co_await sendErrorResponse(managarm::posix::Errors::NAME_TOO_LONG);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}

				logRequest(logRequests || logPaths, "OPENAT", "create '{}'",
					ViewPath{resolver.currentView(), resolver.currentLink()}
					.getPath(self->fsContext()->getRoot()));

				if (!resolver.hasComponent()) {
					if ((req->flags() & managarm::posix::OpenFlags::OF_RDWR)
					|| (req->flags() & managarm::posix::OpenFlags::OF_WRONLY))
						co_await sendErrorResponse(managarm::posix::Errors::IS_DIRECTORY);
					else
						co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
					continue;
				}

				auto directory = resolver.currentLink()->getTarget();

				auto linkResult = co_await directory->getLinkOrCreate(self.get(), resolver.nextComponent(),
					req->mode() & ~self->fsContext()->getUmask(), req->flags() & managarm::posix::OpenFlags::OF_EXCLUSIVE);
				if (!linkResult) {
					co_await sendErrorResponse(linkResult.error() | toPosixProtoError);
					continue;
				}
				auto link = linkResult.value();
				assert(link);
				auto node = link->getTarget();

				auto fileResult = co_await node->open(resolver.currentView(), std::move(link),
									semantic_flags);
				assert(fileResult);
				file = fileResult.value();
				assert(file);
			}else{
				ResolveFlags resolveFlags = 0;

				if(req->flags() & managarm::posix::OpenFlags::OF_NOFOLLOW)
					resolveFlags |= resolveDontFollow;

				auto resolveResult = co_await resolver.resolve(resolveFlags);
				if(!resolveResult) {
					if(resolveResult.error() == protocols::fs::Error::isDirectory) {
						// TODO: Verify additional constraints for sending EISDIR.
						co_await sendErrorResponse(managarm::posix::Errors::IS_DIRECTORY);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
						co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
						continue;
					} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from resolve()" << std::endl;
						co_return;
					}
				}

				logRequest(logRequests || logPaths, "OPENAT", "open '{}'",
					ViewPath{resolver.currentView(), resolver.currentLink()}
					.getPath(self->fsContext()->getRoot()));

				auto target = resolver.currentLink()->getTarget();
				if(req->flags() & managarm::posix::OpenFlags::OF_DIRECTORY) {
					if(target->getType() != VfsType::directory) {
						co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
						continue;
					}
				}

				if(req->flags() & managarm::posix::OpenFlags::OF_PATH) {
					auto dummyFile = smarter::make_shared<DummyFile>(resolver.currentView(), resolver.currentLink());
					DummyFile::serve(dummyFile);
					file = File::constructHandle(std::move(dummyFile));
				} else {
					// this can only be a symlink if O_NOFOLLOW has been passed
					if(target->getType() == VfsType::symlink) {
						co_await sendErrorResponse(managarm::posix::Errors::SYMBOLIC_LINK_LOOP);
						continue;
					}

					auto fileResult = co_await target->open(resolver.currentView(), resolver.currentLink(), semantic_flags);
					if(!fileResult) {
						if(fileResult.error() == Error::noBackingDevice) {
							co_await sendErrorResponse(managarm::posix::Errors::NO_BACKING_DEVICE);
							continue;
						} else if(fileResult.error() == Error::illegalArguments) {
							co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
							continue;
						} else {
							std::cout << "posix: Unexpected failure from open()" << std::endl;
							co_return;
						}
					}
					assert(fileResult);
					file = fileResult.value();
				}
			}

			if(!file) {
				co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
				continue;
			}

			if(file->isTerminal() &&
				!(req->flags() & managarm::posix::OpenFlags::OF_NOCTTY) &&
				self->pgPointer()->getSession()->getSessionId() == (pid_t)self->pid() &&
				self->pgPointer()->getSession()->getControllingTerminal() == nullptr) {
				// POSIX 1003.1-2017 11.1.3
				auto cts = co_await file->getControllingTerminal();
				if(!cts) {
					std::cout << "posix: Unable to get controlling terminal (" << (int)cts.error() << ")" << std::endl;
				} else {
					cts.value()->assignSessionOf(self.get());
				}
			}

			if(req->flags() & managarm::posix::OpenFlags::OF_TRUNC) {
				auto result = co_await file->truncate(0);
				assert(result || result.error() == protocols::fs::Error::illegalOperationTarget);
			}
			auto fd = self->fileContext()->attachFile(file,
					req->flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

			managarm::posix::SvrResponse resp;
			if (fd) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd.value());
			} else {
				resp.set_error(fd.error() | toPosixProtoError);
			}

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
			HEL_CHECK(sendResp.error());
			logBragiReply(resp);
		}else if(preamble.id() == bragi::message_id<managarm::posix::CloseRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::CloseRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "CLOSE", "fd={}", req->fd());

			auto closeErr = self->fileContext()->closeFile(req->fd());

			if(closeErr != Error::success) {
				if(closeErr == Error::noSuchFile) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				} else {
					std::cout << "posix: Unhandled error returned from closeFile" << std::endl;
					break;
				}
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
			HEL_CHECK(sendResp.error());
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
		}else if(preamble.id() == bragi::message_id<managarm::posix::Dup2Request>) {
			auto req = bragi::parse_head_only<managarm::posix::Dup2Request>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}
			logRequest(logRequests, "DUP2", "fd={}", req->fd());

			auto file = self->fileContext()->getFile(req->fd());

			managarm::posix::Dup2Response resp;

			if (!file || req->newfd() < 0) {
				resp.set_error(managarm::posix::Errors::NO_SUCH_FD);
				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

				HEL_CHECK(send_resp.error());
				logBragiReply(resp);
				continue;
			}

			if(req->flags()) {
				if(!(req->flags() & O_CLOEXEC)) {
						resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

						auto [send_resp] = co_await helix_ng::exchangeMsgs(
							conversation,
							helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
						);

						HEL_CHECK(send_resp.error());
						continue;
				}
			}
			bool closeOnExec = (req->flags() & O_CLOEXEC);

			std::expected<int, Error> result = req->newfd();
			if(req->fcntl_mode())
				result = self->fileContext()->attachFile(file, closeOnExec, req->newfd());
			else
				result = self->fileContext()->attachFile(req->newfd(), file, closeOnExec)
					.transform([&]() {
						return req->newfd();
					});

			if (result) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(result.value());
			} else {
				resp.set_error(result.error() | toPosixProtoError);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == bragi::message_id<managarm::posix::IsTtyRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::IsTtyRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}
			logRequest(logRequests, "IS_TTY", "fd={}", req->fd());

			auto file = self->fileContext()->getFile(req->fd());
			if(!file) {
				co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
				continue;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_mode(file->isTerminal());

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
			HEL_CHECK(sendResp.error());
			logBragiReply(resp);
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
		}else if(preamble.id() == managarm::posix::UnlinkAtRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::UnlinkAtRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;
			std::shared_ptr<FsLink> target_link;

			if(req->flags()) {
				if(req->flags() & AT_REMOVEDIR) {
					std::cout << "posix: UNLINKAT flag AT_REMOVEDIR handling unimplemented" << std::endl;
				} else {
					std::cout << "posix: UNLINKAT flag handling unimplemented with unknown flag: " << req->flags() << std::endl;
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				}
			}

			if(req->fd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->fd());

				if (!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get());

			auto resolveResult = co_await resolver.resolve(resolveDontFollow);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::isDirectory) {
					// TODO: Only when AT_REMOVEDIR is not specified, fix this when flag handling is implemented.
					co_await sendErrorResponse(managarm::posix::Errors::IS_DIRECTORY);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			logRequest(logRequests || logPaths, "UNLINKAT", "path='{}'",
				ViewPath(resolver.currentView(), resolver.currentLink())
				.getPath(self->fsContext()->getRoot()));

			target_link = resolver.currentLink();

			auto owner = target_link->getOwner();
			if(!owner) {
				co_await sendErrorResponse(managarm::posix::Errors::RESOURCE_IN_USE);
				continue;
			}
			auto result = co_await owner->unlink(target_link->getName());
			if(!result) {
				if(result.error() == Error::noSuchFile) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				}else if(result.error() == Error::directoryNotEmpty) {
					co_await sendErrorResponse(managarm::posix::Errors::DIRECTORY_NOT_EMPTY);
					continue;
				}else{
					std::cout << "posix: Unexpected failure from unlink()" << std::endl;
					co_return;
				}
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
		}else if(preamble.id() == managarm::posix::RmdirRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::RmdirRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			std::shared_ptr<FsLink> target_link;

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(), self->fsContext()->getWorkingDirectory(),
					req->path(), self.get());

			auto resolveResult = co_await resolver.resolve();
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			logRequest(logRequests || logPaths, "RMDIR", "path='{}'",
				ViewPath(resolver.currentView(), resolver.currentLink())
				.getPath(self->fsContext()->getRoot()));

			target_link = resolver.currentLink();

			auto owner = target_link->getOwner();
			auto result = co_await owner->rmdir(target_link->getName());
			if(!result) {
				co_await sendErrorResponse(result.error() | toPosixProtoError);
				continue;
			}

			co_await sendErrorResponse(managarm::posix::Errors::SUCCESS);
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
		}else if(preamble.id() == bragi::message_id<managarm::posix::IoctlFioclexRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::IoctlFioclexRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "FIOCLEX");

			if(self->fileContext()->setDescriptor(req->fd(), true) != Error::success) {
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

			SignalHandler saved_handler;
			if(req.mode()) {
				SignalHandler handler;
				if(req.sig_handler() == uintptr_t(SIG_DFL)) {
					handler.disposition = SignalDisposition::none;
				}else if(req.sig_handler() == uintptr_t(SIG_IGN)) {
					handler.disposition = SignalDisposition::ignore;
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

				saved_handler = self->signalContext()->changeHandler(req.sig_number(), handler);
			}else{
				saved_handler = self->signalContext()->getHandler(req.sig_number());
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

			if(self->pgPointer()->getSession()->getSessionId() == self->pid()) {
				co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
				continue;
			}

			auto session = TerminalSession::initializeNewSession(self.get());

			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_sid(session->getSessionId());

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::NetserverRequest::message_id) {
			auto [pt_msg] = co_await helix_ng::exchangeMsgs(conversation, helix_ng::RecvInline());

			HEL_CHECK(pt_msg.error());

			logRequest(logRequests, "NETSERVER_REQUEST", "ioctl");

			auto pt_preamble = bragi::read_preamble(pt_msg);

			auto [offer, recv_resp] = co_await [&pt_preamble, &pt_msg, &conversation]() -> async::result<std::pair<helix_ng::OfferResult, helix_ng::RecvInlineResult>> {
				std::vector<uint8_t> pt_tail(pt_preamble.tail_size());
				auto [recv_tail] = co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::recvBuffer(pt_tail.data(), pt_tail.size())
					);
				HEL_CHECK(recv_tail.error());

				auto [offer, send_req, send_tail, recv_resp] = co_await helix_ng::exchangeMsgs(
					co_await net::getNetLane(),
					helix_ng::offer(
						helix_ng::want_lane,
						helix_ng::sendBuffer(pt_msg.data(), pt_msg.size()),
						helix_ng::sendBuffer(pt_tail.data(), pt_tail.size()),
						helix_ng::recvInline()
					)
				);
				HEL_CHECK(offer.error());
				HEL_CHECK(send_req.error());
				HEL_CHECK(send_tail.error());
				HEL_CHECK(recv_resp.error());
				co_return {std::move(offer), recv_resp};
			}();

			auto recv_preamble = bragi::read_preamble(recv_resp);
			assert(!recv_preamble.error());

			if(recv_preamble.id() == managarm::fs::IfreqReply::message_id) {
				std::vector<uint8_t> tail(recv_preamble.tail_size());
				auto [recv_tail] = co_await helix_ng::exchangeMsgs(
						offer.descriptor(),
						helix_ng::recvBuffer(tail.data(), tail.size())
					);
				HEL_CHECK(recv_tail.error());

				auto resp = *bragi::parse_head_tail<managarm::fs::IfreqReply>(recv_resp, tail);

				auto [send_resp, send_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
				);

				HEL_CHECK(send_resp.error());
				HEL_CHECK(send_tail.error());
			} else if(recv_preamble.id() == managarm::fs::IfconfReply::message_id) {
				std::vector<uint8_t> tail(recv_preamble.tail_size());
				auto [recv_tail] = co_await helix_ng::exchangeMsgs(
						offer.descriptor(),
						helix_ng::recvBuffer(tail.data(), tail.size())
					);
				HEL_CHECK(recv_tail.error());

				auto resp = *bragi::parse_head_tail<managarm::fs::IfconfReply>(recv_resp, tail);

				auto [send_resp, send_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
				);

				HEL_CHECK(send_resp.error());
				HEL_CHECK(send_tail.error());
			} else {
				std::cout << "posix: unexpected message in netserver forward" << std::endl;
				break;
			}
		}else if(preamble.id() == managarm::posix::SocketRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SocketRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "SOCKET");

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			if(req->flags() & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			smarter::shared_ptr<File, FileHandle> file;
			if(req->domain() == AF_UNIX) {
				if(req->socktype() != SOCK_DGRAM && req->socktype() != SOCK_STREAM
				&& req->socktype() != SOCK_SEQPACKET) {
					std::println("posix: unexpected socket type {:#x}", req->socktype());
					co_await sendErrorResponse(managarm::posix::Errors::UNSUPPORTED_SOCKET_TYPE);
					continue;
				}

				if(req->protocol()) {
					std::println("posix: unexpected protocol {:#x} for socket", req->protocol());
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}

				auto un = un_socket::createSocketFile(req->flags() & SOCK_NONBLOCK, req->socktype());

				if(!un) {
					co_await sendErrorResponse(un.error() | toPosixProtoError);
					continue;
				}

				file = std::move(un.value());
			}else if(req->domain() == AF_NETLINK) {
				assert(req->socktype() == SOCK_RAW || req->socktype() == SOCK_DGRAM);
				// NL_ROUTE gets handled by the netserver.
				if(req->protocol() == NETLINK_ROUTE)
					file = co_await extern_socket::createSocket(
						co_await net::getNetLane(),
						req->domain(),
						req->socktype(), req->protocol(),
						req->flags() & SOCK_NONBLOCK
					);
				else if(netlink::nl_socket::protocol_supported(req->protocol()))
					file = netlink::nl_socket::createSocketFile(req->protocol(), req->socktype(), req->flags() & SOCK_NONBLOCK);
				else {
					std::cout << std::format("posix: unhandled netlink protocol 0x{:X}",
						req->protocol()) << std::endl;
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
			} else if (req->domain() == AF_INET || req->domain() == AF_PACKET) {
				file = co_await extern_socket::createSocket(
					co_await net::getNetLane(),
					req->domain(),
					req->socktype(), req->protocol(),
					req->flags() & SOCK_NONBLOCK);
			}else{
				std::cout << "posix: SOCKET: Handle unknown protocols families, this is: " << req->domain() << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			auto fd = self->fileContext()->attachFile(file,
					req->flags() & SOCK_CLOEXEC);

			if (fd) {
				resp.set_fd(fd.value());
			} else {
				resp.set_error(fd.error() | toPosixProtoError);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::SockpairRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SockpairRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "SOCKPAIR");

			assert(!(req->flags() & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)));

			if(req->flags() & SOCK_NONBLOCK)
				std::cout << "\e[31mposix: socketpair(SOCK_NONBLOCK)"
						" is not implemented correctly\e[39m" << std::endl;

			if(req->domain() != AF_UNIX) {
				std::cout << "\e[31mposix: socketpair() with domain " << req->domain() <<
						" is not implemented correctly\e[39m" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ADDRESS_FAMILY_NOT_SUPPORTED);
				continue;
			}
			if(req->socktype() != SOCK_DGRAM && req->socktype() != SOCK_STREAM
					&& req->socktype() != SOCK_SEQPACKET) {
				std::cout << "\e[31mposix: socketpair() with socktype " << req->socktype() <<
						" is not implemented correctly\e[39m" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}
			if(req->protocol() && req->protocol() != PF_UNSPEC) {
				std::cout << "\e[31mposix: socketpair() with protocol " << req->protocol() <<
						" is not implemented correctly\e[39m" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::PROTOCOL_NOT_SUPPORTED);
				continue;
			}

			auto pair = un_socket::createSocketPair(self.get(), req->flags() & SOCK_NONBLOCK, req->socktype());
			auto fd0 = self->fileContext()->attachFile(std::get<0>(pair),
					req->flags() & SOCK_CLOEXEC);
			auto fd1 = self->fileContext()->attachFile(std::get<1>(pair),
					req->flags() & SOCK_CLOEXEC);

			managarm::posix::SvrResponse resp;
			if (fd0 && fd1) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.add_fds(fd0.value());
				resp.add_fds(fd1.value());
			} else {
				resp.set_error((!fd0 ? fd0.error() : fd1.error()) | toPosixProtoError);
				if (fd0)
					self->fileContext()->closeFile(fd0.value());
				if (fd1)
					self->fileContext()->closeFile(fd1.value());
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::AcceptRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::AcceptRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "ACCEPT", "fd={}", req->fd());

			auto sockfile = self->fileContext()->getFile(req->fd());
			if(!sockfile) {
				co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
				continue;
			}

			auto newfileResult = co_await sockfile->accept(self.get());
			if(!newfileResult) {
				co_await sendErrorResponse(newfileResult.error() | toPosixProtoError);
				continue;
			}
			auto newfile = newfileResult.value();
			auto fd = self->fileContext()->attachFile(std::move(newfile));

			managarm::posix::SvrResponse resp;
			if (fd) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd.value());
			} else {
				resp.set_error(fd.error() | toPosixProtoError);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(req.request_type() == managarm::posix::CntReqType::EPOLL_CALL) {
			logRequest(logRequests, "EPOLL_CALL");

			// Since file descriptors may appear multiple times in a poll() call,
			// we need to de-duplicate them here.
			std::unordered_map<int, unsigned int> fdsToEvents;

			auto epfile = epoll::createFile();
			assert(req.fds_size() == req.events_size());

			bool errorOut = false;
			for(size_t i = 0; i < req.fds_size(); i++) {
				auto [mapIt, inserted] = fdsToEvents.insert({req.fds(i), 0});
				if(!inserted)
					continue;

				auto file = self->fileContext()->getFile(req.fds(i));
				if(!file) {
					// poll() is supposed to fail on a per-FD basis.
					mapIt->second = POLLNVAL;
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
			}
			if(errorOut)
				continue;

			struct epoll_event events[16];
			size_t k;

			{
				auto cancelEvent = self->cancelEventRegistry().event(self->credentials(), req.cancellation_id());
				if (!cancelEvent) {
					std::println("posix: possibly duplicate cancellation ID registered");
					sendErrorResponse(managarm::posix::Errors::INTERNAL_ERROR);
					continue;
				}

				if(req.timeout() < 0) {
					k = co_await epoll::wait(epfile.get(), events, 16, cancelEvent);
				}else if(!req.timeout()) {
					// Do not bother to set up a timer for zero timeouts.
					async::cancellation_event cancel_wait;
					cancel_wait.cancel();
					k = co_await epoll::wait(epfile.get(), events, 16, cancel_wait);
				}else{
					assert(req.timeout() > 0);
					co_await async::race_and_cancel(
						async::lambda([&](auto c) -> async::result<void> {
							co_await helix::sleepFor(static_cast<uint64_t>(req.timeout()), c);
						}),
						async::lambda([&](auto c) -> async::result<void> {
							co_await async::suspend_indefinitely(c, cancelEvent);
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
			resp.set_error(managarm::posix::Errors::SUCCESS);

			for(size_t i = 0; i < req.fds_size(); ++i) {
				auto it = fdsToEvents.find(req.fds(i));
				assert(it != fdsToEvents.end());
				resp.add_events(it->second);
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
		}else if(preamble.id() == bragi::message_id<managarm::posix::TimerFdCreateRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::TimerFdCreateRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "TIMER_FD_CREATE");

			if(req->flags() & ~(TFD_CLOEXEC | TFD_NONBLOCK)) {
				std::println("posix: Unsupported flags {} for timerfd_create()", req->flags());
				co_await sendErrorResponse.operator()<managarm::posix::TimerFdCreateResponse>(
					managarm::posix::Errors::ILLEGAL_ARGUMENTS
				);
				continue;
			}

			if (req->clock() != CLOCK_MONOTONIC && req->clock() != CLOCK_REALTIME) {
				std::println("posix: timerfd is not supported for clock {}", req->clock());
				co_await sendErrorResponse.operator()<managarm::posix::TimerFdCreateResponse>(
					managarm::posix::Errors::ILLEGAL_ARGUMENTS
				);
				continue;
			}

			auto file = timerfd::createFile(req->clock(), req->flags() & TFD_NONBLOCK);
			auto fd = self->fileContext()->attachFile(file, req->flags() & TFD_CLOEXEC);

			managarm::posix::TimerFdCreateResponse resp;
			if (fd) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd.value());
			} else {
				resp.set_error(fd.error() | toPosixProtoError);
			}

			auto ser = resp.SerializeAsString();
			auto [sendResp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}));
			HEL_CHECK(sendResp.error());
			logBragiReply(resp);
		}else if(preamble.id() == bragi::message_id<managarm::posix::TimerFdSetRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::TimerFdSetRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "TIMER_FD_SET");

			auto file = self->fileContext()->getFile(req->fd());
			if (!file) {
				co_await sendErrorResponse.operator()<managarm::posix::TimerFdSetResponse>(
					managarm::posix::Errors::NO_SUCH_FD
				);
				continue;
			} else if(file->kind() != FileKind::timerfd) {
				co_await sendErrorResponse.operator()<managarm::posix::TimerFdSetResponse>(
					managarm::posix::Errors::ILLEGAL_ARGUMENTS
				);
				continue;
			}
			timespec initial = {};
			timespec interval = {};
			timerfd::getTime(file.get(), initial, interval);
			timerfd::setTime(file.get(), req->flags(),
					{static_cast<time_t>(req->value_sec()), static_cast<long>(req->value_nsec())},
					{static_cast<time_t>(req->interval_sec()), static_cast<long>(req->interval_nsec())});

			managarm::posix::TimerFdSetResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_value_sec(initial.tv_sec);
			resp.set_value_nsec(initial.tv_nsec);
			resp.set_interval_sec(interval.tv_sec);
			resp.set_interval_nsec(interval.tv_nsec);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}));
			HEL_CHECK(sendResp.error());
			logBragiReply(resp);
		}else if(preamble.id() == bragi::message_id<managarm::posix::TimerFdGetRequest>) {
			auto req = bragi::parse_head_only<managarm::posix::TimerFdGetRequest>(recv_head);
			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "TIMER_FD_GET");

			auto file = self->fileContext()->getFile(req->fd());
			if (!file) {
				co_await sendErrorResponse.operator()<managarm::posix::TimerFdGetResponse>(
					managarm::posix::Errors::NO_SUCH_FD
				);
				continue;
			} else if(file->kind() != FileKind::timerfd) {
				co_await sendErrorResponse.operator()<managarm::posix::TimerFdGetResponse>(
					managarm::posix::Errors::ILLEGAL_ARGUMENTS
				);
				continue;
			}
			timespec initial = {};
			timespec interval = {};
			timerfd::getTime(file.get(), initial, interval);

			managarm::posix::TimerFdGetResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_value_sec(initial.tv_sec);
			resp.set_value_nsec(initial.tv_nsec);
			resp.set_interval_sec(interval.tv_sec);
			resp.set_interval_nsec(interval.tv_nsec);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}));
			HEL_CHECK(sendResp.error());
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
		}else if(preamble.id() == managarm::posix::InotifyCreateRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::InotifyCreateRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "INOTIFY_CREATE");

			if(req->flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC | managarm::posix::OpenFlags::OF_NONBLOCK)) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			auto file = inotify::createFile(req->flags() & managarm::posix::OpenFlags::OF_NONBLOCK);
			auto fd = self->fileContext()->attachFile(file,
					req->flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

			managarm::posix::SvrResponse resp;
			if (fd) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd.value());
			} else {
				resp.set_error(fd.error() | toPosixProtoError);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::InotifyAddRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::InotifyAddRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			managarm::posix::SvrResponse resp;

			logRequest(logRequests || logPaths, "INOTIFY_ADD");

			auto ifile = self->fileContext()->getFile(req->fd());
			if(!ifile) {
				co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
				continue;
			} else if(ifile->kind() != FileKind::inotify) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			ResolveFlags flags = 0;

			if(req->flags() & IN_DONT_FOLLOW)
				flags |= resolveDontFollow;

			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					self->fsContext()->getWorkingDirectory(), req->path(), self.get());
			auto resolveResult = co_await resolver.resolve(flags);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			auto wd = inotify::addWatch(ifile.get(), resolver.currentLink()->getTarget(),
					req->flags());

			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_wd(wd);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::InotifyRmRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::InotifyRmRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests || logPaths, "INOTIFY_RM");
			managarm::posix::InotifyRmReply resp;

			auto ifile = self->fileContext()->getFile(req->ifd());
			if(!ifile) {
				resp.set_error(managarm::posix::Errors::BAD_FD);
				continue;
			} else if(ifile->kind() != FileKind::inotify) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			if(inotify::removeWatch(ifile.get(), req->wd()))
				resp.set_error(managarm::posix::Errors::SUCCESS);
			else
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::EventfdCreateRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::EventfdCreateRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "EVENTFD_CREATE");

			managarm::posix::SvrResponse resp;

			if (req->flags() & ~(managarm::posix::EventFdFlags::CLOEXEC
					| managarm::posix::EventFdFlags::NONBLOCK
					| managarm::posix::EventFdFlags::SEMAPHORE)) {
				std::cout << "posix: invalid flag specified" << std::endl;
				std::cout << "posix: flags specified: " << req->flags() << std::endl;
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			} else {
				auto file = eventfd::createFile(req->initval(),
					req->flags() & managarm::posix::EventFdFlags::NONBLOCK,
					req->flags() & managarm::posix::EventFdFlags::SEMAPHORE);
				auto fd = self->fileContext()->attachFile(file,
						req->flags() & managarm::posix::EventFdFlags::CLOEXEC);

				if (fd) {
					resp.set_error(managarm::posix::Errors::SUCCESS);
					resp.set_fd(fd.value());
				} else {
					resp.set_error(fd.error() | toPosixProtoError);
				}
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::MknodAtRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::MknodAtRequest>(recv_head, tail);

			if(!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests || logPaths, "MKNODAT", "path='{}' mode={:o} device={:#x}", req->path(), req->mode(), req->device());

			managarm::posix::SvrResponse resp;

			ViewPath relative_to;
			smarter::shared_ptr<File, FileHandle> file;

			if(!req->path().size()) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			if(req->dirfd() == AT_FDCWD) {
				relative_to = self->fsContext()->getWorkingDirectory();
			} else {
				file = self->fileContext()->getFile(req->dirfd());

				if(!file) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_FD);
					continue;
				}

				relative_to = {file->associatedMount(), file->associatedLink()};
			}

			// TODO: Add resolveNoTrailingSlash if not making a directory?
			PathResolver resolver;
			resolver.setup(self->fsContext()->getRoot(),
					relative_to, req->path(), self.get());
			auto resolveResult = co_await resolver.resolve(resolvePrefix);
			if(!resolveResult) {
				if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
					co_await sendErrorResponse(managarm::posix::Errors::FILE_NOT_FOUND);
					continue;
				} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
					co_await sendErrorResponse(managarm::posix::Errors::NOT_A_DIRECTORY);
					continue;
				} else {
					std::cout << "posix: Unexpected failure from resolve()" << std::endl;
					co_return;
				}
			}

			auto parent = resolver.currentLink()->getTarget();
			auto existsResult = co_await parent->getLink(resolver.nextComponent());
			if (existsResult) {
				co_await sendErrorResponse(managarm::posix::Errors::ALREADY_EXISTS);
				continue;
			}

			VfsType type;
			DeviceId dev;
			if(S_ISDIR(req->mode())) {
				type = VfsType::directory;
			} else if(S_ISCHR(req->mode())) {
				type = VfsType::charDevice;
			} else if(S_ISBLK(req->mode())) {
				type = VfsType::blockDevice;
			} else if(S_ISREG(req->mode())) {
				type = VfsType::regular;
			} else if(S_ISFIFO(req->mode())) {
				type = VfsType::fifo;
			} else if(S_ISLNK(req->mode())) {
				type = VfsType::symlink;
			} else if(S_ISSOCK(req->mode())) {
				type = VfsType::socket;
			} else {
				type = VfsType::null;
			}

			// TODO: Verify the proper error return here.
			if(type == VfsType::charDevice || type == VfsType::blockDevice) {
				dev.first = major(req->device());
				dev.second = minor(req->device());

				auto result = co_await parent->mkdev(resolver.nextComponent(), type, dev);
				if(!result) {
					if(result.error() == Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from mkdev()" << std::endl;
						co_return;
					}
				}
			} else if(type == VfsType::fifo) {
				auto result = co_await parent->mkfifo(
					resolver.nextComponent(),
					req->mode() & ~self->fsContext()->getUmask()
				);
				if(!result) {
					if(result.error() == Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from mkfifo()" << std::endl;
						co_return;
					}
				}
			} else if(type == VfsType::socket) {
				auto result = co_await parent->mksocket(resolver.nextComponent());
				if(!result) {
					if(result.error() == Error::illegalOperationTarget) {
						co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
						continue;
					} else {
						std::cout << "posix: Unexpected failure from mksocket()" << std::endl;
						co_return;
					}
				}
			} else {
				// TODO: Handle regular files.
				std::cout << "\e[31mposix: Creating regular files with mknod is not supported.\e[39m" << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::GetPgidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetPgidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "GET_PGID");

			std::shared_ptr<Process> target;
			if(req->pid()) {
				target = Process::findProcess(req->pid());
				if(!target) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_RESOURCE);
					continue;
				}
			} else {
				target = self;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(target->pgPointer()->getHull()->getPid());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::SetPgidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetPgidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "SET_PGID");

			std::shared_ptr<Process> target;
			if(req->pid()) {
				target = Process::findProcess(req->pid());
				if(!target) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_RESOURCE);
					continue;
				}
			} else {
				target = self;
			}

			if(target->pgPointer()->getSession() != self->pgPointer()->getSession()) {
				co_await sendErrorResponse(managarm::posix::Errors::INSUFFICIENT_PERMISSION);
				continue;
			}

			// We can't change the process group ID of the session leader
			if(target->pid() == target->pgPointer()->getSession()->getSessionId()) {
				co_await sendErrorResponse(managarm::posix::Errors::INSUFFICIENT_PERMISSION);
				continue;
			}

			if(target->getParent()->pid() == self->pid()) {
				if(target->didExecute()) {
					co_await sendErrorResponse(managarm::posix::Errors::ACCESS_DENIED);
					continue;
				}
			}

			std::shared_ptr<ProcessGroup> group;
			// If pgid is 0, we're going to set it to the calling process's pid, use the target group.
			if(req->pgid()) {
				group = target->pgPointer()->getSession()->getProcessGroupById(req->pgid());
			} else {
				group = target->pgPointer();
			}

			if(group) {
				// Found, do permission checking and join
				group->reassociateProcess(target.get());
			} else {
				// Not found, making it if pgid and pid match, or if pgid is 0, indicating that we should make one
				if(target->pid() == req->pgid() || !req->pgid()) {
					target->pgPointer()->getSession()->spawnProcessGroup(target.get());
				} else {
					// Doesn't exists, and not requesting a make?
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_RESOURCE);
					continue;
				}
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::GetSidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetSidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "GET_SID", "pid={}", req->pid());

			std::shared_ptr<Process> target;
			if(req->pid()) {
				target = Process::findProcess(req->pid());
				if(!target) {
					co_await sendErrorResponse(managarm::posix::Errors::NO_SUCH_RESOURCE);
					continue;
				}
			} else {
				target = self;
			}
			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(target->pgPointer()->getSession()->getSessionId());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::MemFdCreateRequest::message_id) {
			managarm::posix::SvrResponse resp;
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::MemFdCreateRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "MEMFD_CREATE", "'{}'", req->name());

			if(req->flags() & ~(MFD_CLOEXEC | MFD_ALLOW_SEALING)) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			auto link = SpecialLink::makeSpecialLink(VfsType::regular, 0777);
			auto memFile = smarter::make_shared<MemoryFile>(nullptr, link, (req->flags() & MFD_ALLOW_SEALING) == true);
			MemoryFile::serve(memFile);
			auto file = File::constructHandle(std::move(memFile));

			int flags = 0;

			if(req->flags() & MFD_CLOEXEC) {
				flags |= managarm::posix::OpenFlags::OF_CLOEXEC;
			}

			auto fd = self->fileContext()->attachFile(file, flags);

			if (fd) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd.value());
			} else {
				resp.set_error(fd.error() | toPosixProtoError);
			}

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
			HEL_CHECK(sendResp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::SetAffinityRequest::message_id) {
			std::vector<uint8_t> tail(preamble.tail_size());
			auto [recv_tail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recv_tail.error());

			logBragiRequest(tail);
			auto req = bragi::parse_head_tail<managarm::posix::SetAffinityRequest>(recv_head, tail);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "SET_AFFINITY");

			auto handle = self->threadDescriptor().getHandle();

			if(self->pid() != req->pid()) {
				// TODO: permission checking
				auto target_process = self->findProcess(req->pid());
				if(target_process == nullptr) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
				handle = target_process->threadDescriptor().getHandle();
			}

			HelError e = helSetAffinity(handle, req->mask().data(), req->mask().size());

			if(e == kHelErrIllegalArgs) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			} else if(e != kHelErrNone) {
				std::cout << "posix: SET_AFFINITY hel call returned unexpected error: " << e << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::INTERNAL_ERROR);
				continue;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::GetAffinityRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetAffinityRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "GET_AFFINITY");

			if(!req->size()) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			std::vector<uint8_t> affinity(req->size());

			auto handle = self->threadDescriptor().getHandle();

			if(req->pid() && self->pid() != req->pid()) {
				// TODO: permission checking
				auto target_process = self->findProcess(req->pid());
				if(target_process == nullptr) {
					co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					continue;
				}
				handle = target_process->threadDescriptor().getHandle();
			}

			size_t actual_size;
			HelError e = helGetAffinity(handle, affinity.data(), req->size(), &actual_size);

			if(e == kHelErrBufferTooSmall) {
				co_await sendErrorResponse(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			} else if(e != kHelErrNone) {
				std::cout << "posix: GET_AFFINITY hel call returned unexpected error: " << e << std::endl;
				co_await sendErrorResponse(managarm::posix::Errors::INTERNAL_ERROR);
				continue;
			}

			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(self->pid());

			auto [sendResp, sendData] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(affinity.data(), affinity.size())
			);
			HEL_CHECK(sendResp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::GetMemoryInformationRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::GetMemoryInformationRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "GET_MEMORY_INFORMATION");

			managarm::kerncfg::GetMemoryInformationRequest kerncfgRequest;
			auto [offer, kerncfgSendResp, kerncfgResp] = co_await helix_ng::exchangeMsgs(
				getKerncfgLane(),
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(kerncfgRequest, frg::stl_allocator{}),
					helix_ng::recvInline()
				)
			);
			HEL_CHECK(offer.error());
			HEL_CHECK(kerncfgSendResp.error());
			HEL_CHECK(kerncfgResp.error());

			auto kernResp = bragi::parse_head_only<managarm::kerncfg::GetMemoryInformationResponse>(kerncfgResp);
			kerncfgResp.reset();

			managarm::posix::GetMemoryInformationResponse resp;
			resp.set_total_usable_memory(kernResp->total_usable_memory());
			resp.set_available_memory(kernResp->available_memory());
			resp.set_memory_unit(kernResp->memory_unit());

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(sendResp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::SysconfRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SysconfRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "SYSCONF");

			managarm::posix::SysconfResponse resp;

			// Configured == available == online
			if(req->num() == _SC_NPROCESSORS_CONF || req->num() == _SC_NPROCESSORS_ONLN) {
				managarm::kerncfg::GetNumCpuRequest kerncfgRequest;
				auto [offer, kerncfgSendResp, kerncfgResp] = co_await helix_ng::exchangeMsgs(
				getKerncfgLane(),
				helix_ng::offer(
						helix_ng::sendBragiHeadOnly(kerncfgRequest, frg::stl_allocator{}),
						helix_ng::recvInline()
					)
				);
				HEL_CHECK(offer.error());
				HEL_CHECK(kerncfgSendResp.error());
				HEL_CHECK(kerncfgResp.error());

				auto kernResp = bragi::parse_head_only<managarm::kerncfg::GetNumCpuResponse>(kerncfgResp);
				kerncfgResp.reset();

				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_value(kernResp->num_cpu());
			} else {
				// Not handled, bubble up EINVAL.
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			}
			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::ParentDeathSignalRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::ParentDeathSignalRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			self->setParentDeathSignal(req->signal() ? std::optional{req->signal()} : std::nullopt);

			managarm::posix::ParentDeathSignalResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::ProcessDumpableRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::ProcessDumpableRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			managarm::posix::ProcessDumpableResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			if(req->set()) {
				self->setDumpable(req->new_value());
			}

			resp.set_value(self->getDumpable());

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::SetIntervalTimerRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetIntervalTimerRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			managarm::posix::SetIntervalTimerResponse resp;
			if(req->which() == ITIMER_REAL) {
				logRequest(logRequests, "SETITIMER", "value={}.{:06d}s interval={}.{:06d}s",
					req->value_sec(), req->value_usec(), req->interval_sec(), req->interval_usec());

				uint64_t value = 0;
				uint64_t interval = 0;
				if(self->realTimer)
					self->realTimer->getTime(value, interval);

				resp.set_value_sec(value / 1'000'000'000);
				resp.set_value_usec((value % 1'000'000'000) / 1'000);
				resp.set_interval_sec(interval / 1'000'000'000);
				resp.set_interval_usec((interval % 1'000'000'000) / 1'000);

				if(self->realTimer)
					self->realTimer->cancel();
				self->realTimer = std::make_shared<Process::IntervalTimer>(self,
						req->value_sec() * 1'000'000'000 + req->value_usec() * 1'000,
						req->interval_sec() * 1'000'000'000 + req->interval_usec() * 1'000);
				self->realTimer->arm(self->realTimer);

				resp.set_error(managarm::posix::Errors::SUCCESS);
			} else {
				// TODO: handle ITIMER_VIRTUAL and ITIMER_PROF
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				std::cout << "posix: ITIMER_VIRTUAL and ITIMER_PROF are unsupported" << std::endl;
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		} else if (preamble.id() == managarm::posix::UmaskRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::UmaskRequest>(recv_head);
			logRequest(logRequests, "UMASK", "newmask={:o}", req->newmask());

			managarm::posix::UmaskResponse resp;
			mode_t oldmask = self->fsContext()->setUmask(req->newmask());
			resp.set_oldmask(oldmask);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::TimerCreateRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::TimerCreateRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "TIMER_CREATE", "clockid={}", req->clockid());

			managarm::posix::TimerCreateResponse resp;
			if(req->clockid() == CLOCK_MONOTONIC || req->clockid() == CLOCK_REALTIME) {
				auto id = self->timerIdAllocator.allocate();
				assert(!self->timers.contains(id));

				self->timers[id] = std::make_shared<Process::PosixTimerContext>(
					req->clockid(),
					nullptr,
					req->sigev_tid() ? std::optional{req->sigev_tid()} : std::nullopt,
					req->sigev_signo()
				);

				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_timer_id(id);
			} else {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				std::println("posix: unsupported clock_id {}", req->clockid());
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::TimerSetRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::TimerSetRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "TIMER_SET", "timer={}", req->timer());

			managarm::posix::TimerSetResponse resp;
			resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

			if(self->timers.contains(req->timer())) {
				auto timerContext = self->timers[req->timer()];

				uint64_t value = 0;
				uint64_t interval = 0;
				if(timerContext->timer)
					timerContext->timer->getTime(value, interval);

				resp.set_value_sec(value / 1'000'000'000);
				resp.set_value_nsec(value % 1'000'000'000);
				resp.set_interval_sec(interval / 1'000'000'000);
				resp.set_interval_nsec(interval % 1'000'000'000);

				if(timerContext->timer)
					timerContext->timer->cancel();

				auto targetProcess = self;
				if(timerContext->tid && targetProcess->tid() != *timerContext->tid)
					targetProcess = Process::findProcess(*timerContext->tid);

				if(targetProcess) {
					uint64_t valueNanos = 0;
					uint64_t intervalNanos = 0;

					if(req->value_sec() || req->value_nsec()) {
						valueNanos = posix::convertToNanos(
							{static_cast<time_t>(req->value_sec()), static_cast<long>(req->value_nsec())},
							timerContext->clockid, !(req->flags() & TFD_TIMER_ABSTIME));
						intervalNanos = posix::convertToNanos(
							{static_cast<time_t>(req->interval_sec()), static_cast<long>(req->interval_nsec())},
							CLOCK_MONOTONIC);
					}

					timerContext->timer = std::make_shared<Process::PosixTimer>(targetProcess,
						timerContext->tid, timerContext->signo, req->timer(), valueNanos, intervalNanos);
					timerContext->timer->arm(timerContext->timer);
					resp.set_error(managarm::posix::Errors::SUCCESS);
				}
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::TimerGetRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::TimerGetRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "TIMER_GET", "timer={}", req->timer());

			managarm::posix::TimerGetResponse resp;
			if(self->timers.contains(req->timer())) {
				auto timerContext = self->timers[req->timer()];
				resp.set_error(managarm::posix::Errors::SUCCESS);

				uint64_t value = 0;
				uint64_t interval = 0;
				if(timerContext->timer)
					timerContext->timer->getTime(value, interval);

				resp.set_value_sec(value / 1'000'000'000);
				resp.set_value_nsec(value % 1'000'000'000);
				resp.set_interval_sec(interval / 1'000'000'000);
				resp.set_interval_nsec(interval % 1'000'000'000);
			} else {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::TimerDeleteRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::TimerDeleteRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			logRequest(logRequests, "TIMER_DELETE", "timer={}", req->timer());

			managarm::posix::TimerDeleteResponse resp;
			if(self->timers.contains(req->timer())) {
				auto ctx = self->timers[req->timer()];
				if(ctx->timer) {
					ctx->timer->cancel();
					ctx->timer = nullptr;
				}
				self->timers.erase(req->timer());
				self->timerIdAllocator.free(req->timer());
				resp.set_error(managarm::posix::Errors::SUCCESS);
			} else {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::PidfdOpenRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::PidfdOpenRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			auto proc = Process::findProcess(req->pid());
			if(!proc) {
				co_await sendErrorResponse.template operator()<managarm::posix::PidfdOpenResponse>
					(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			auto pidfd = createPidfdFile(proc, req->flags() & PIDFD_NONBLOCK);
			auto fd = self->fileContext()->attachFile(pidfd, req->flags() & PIDFD_NONBLOCK);

			managarm::posix::PidfdOpenResponse resp;
			if (fd) {
				resp.set_error(managarm::posix::Errors::SUCCESS);
				resp.set_fd(fd.value());
			} else {
				resp.set_error(fd.error() | toPosixProtoError);
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::PidfdSendSignalRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::PidfdSendSignalRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			auto fd = self->fileContext()->getFile(req->pidfd());
			if(!fd || fd->kind() != FileKind::pidfd) {
				co_await sendErrorResponse.template operator()<managarm::posix::PidfdSendSignalResponse>
					(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			auto pid = smarter::static_pointer_cast<pidfd::OpenFile>(fd)->pid();
			if(pid <= 0) {
				co_await sendErrorResponse.template operator()<managarm::posix::PidfdSendSignalResponse>
					(managarm::posix::Errors::NO_SUCH_RESOURCE);
				continue;
			}

			auto target = Process::findProcess(pid);
			if(!target) {
				co_await sendErrorResponse.template operator()<managarm::posix::PidfdSendSignalResponse>
					(managarm::posix::Errors::NO_SUCH_RESOURCE);
				continue;
			}

			UserSignal info;
			info.pid = self->pid();
			info.uid = 0;
			target->signalContext()->issueSignal(req->signal(), info);

			managarm::posix::PidfdSendSignalResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::PidfdGetPidRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::PidfdGetPidRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			auto fd = self->fileContext()->getFile(req->pidfd());
			if(!fd || fd->kind() != FileKind::pidfd) {
				co_await sendErrorResponse.template operator()<managarm::posix::PidfdGetPidResponse>
					(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				continue;
			}

			auto pid = smarter::static_pointer_cast<pidfd::OpenFile>(fd)->pid();
			if(pid <= 0) {
				co_await sendErrorResponse.template operator()<managarm::posix::PidfdGetPidResponse>
					(managarm::posix::Errors::NO_SUCH_RESOURCE);
				continue;
			}

			managarm::posix::PidfdGetPidResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_pid(pid);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
		}else if(preamble.id() == managarm::posix::SetResourceLimitRequest::message_id) {
			auto req = bragi::parse_head_only<managarm::posix::SetResourceLimitRequest>(recv_head);

			if (!req) {
				std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
				break;
			}

			managarm::posix::SetResourceLimitResponse resp;
			resp.set_error(managarm::posix::Errors::SUCCESS);

			switch(req->resource()) {
				case RLIMIT_NOFILE:
					self->fileContext()->setFdLimit(req->max());
					break;
				default:
					resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					break;
			}

			auto [send_resp] = co_await helix_ng::exchangeMsgs(
				conversation,
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

		if (preamble.id() == managarm::posix::CntRequest::message_id) {
			if(posix::ostContext.isActive()) {
				posix::ostContext.emit(
					posix::ostEvtLegacyRequest,
					posix::ostAttrRequest(req.request_type()),
					posix::ostAttrTime(timer.elapsed())
				);
			}
		} else {
			if(posix::ostContext.isActive()) {
				posix::ostContext.emit(
					posix::ostEvtRequest,
					posix::ostAttrRequest(preamble.id()),
					posix::ostAttrTime(timer.elapsed())
				);
			}
		}
	}

	if(logCleanup)
		std::cout << "\e[33mposix: Exiting serveRequests()\e[39m" << std::endl;
	generation->requestsDone.raise();
}
