#include <format>
#include <print>
#include <set>
#include <limits.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include <frg/scope_exit.hpp>

#include "common.hpp"
#include "../epoll.hpp"

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::CntRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	frg::scope_exit traceOnExit{[&] {
		if(posix::ostContext.isActive()) {
			posix::ostContext.emit(
				posix::ostEvtLegacyRequest,
				posix::ostAttrRequest(req.request_type()),
				posix::ostAttrTime(timer.elapsed())
			);
		}
	}};

	if(req.request_type() == managarm::posix::CntReqType::WAIT) {
		if(req.flags() & ~(WNOHANG | WUNTRACED | WCONTINUED)) {
			std::cout << "posix: WAIT invalid flags: " << req.flags() << std::endl;
			co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}

		WaitFlags flags = waitExited;

		if(req.flags() & WNOHANG)
			flags |= waitNonBlocking;

		if(req.flags() & WUNTRACED)
			std::cout << "\e[31mposix: WAIT flag WUNTRACED is silently ignored\e[39m" << std::endl;

		if(req.flags() & WCONTINUED)
			std::cout << "\e[31mposix: WAIT flag WCONTINUED is silently ignored\e[39m" << std::endl;

		logRequest(logRequests, self, "WAIT", "pid={}", req.pid());

		frg::expected<Error, Process::WaitResult> waitResult = Error::ioError;

		{
			auto cancelEvent = self->cancelEventRegistry().event(self->credentials(), req.cancellation_id());
			if (!cancelEvent) {
				std::println("posix: possibly duplicate cancellation ID registered");
				co_await sendErrorResponse(conversation, managarm::posix::Errors::INTERNAL_ERROR);
				co_return {};
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
		logRequest(logRequests, self, "GET_RESOURCE_USAGE");

		HelThreadStats stats;
		HEL_CHECK(helQueryThreadStats(self->threadDescriptor().getHandle(), &stats));

		int32_t mode = static_cast<int32_t>(req.mode());
		uint64_t user_time;
		if(mode == RUSAGE_SELF) {
			user_time = stats.userTime;
		}else if(mode == RUSAGE_CHILDREN) {
			user_time = self->threadGroup()->accumulatedUsage().userTime;
		}else if(mode == RLIMIT_FSIZE) {
			user_time = RLIM_INFINITY;
		}else{
			std::cout << "\e[31mposix: GET_RESOURCE_USAGE mode is not supported, requested mode: " << mode << "\e[39m"
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
	}else if(req.request_type() == managarm::posix::CntReqType::FCHDIR) {
		logRequest(logRequests, self, "FCHDIR");

		managarm::posix::SvrResponse resp;

		auto file = self->fileContext()->getFile(req.fd());

		if(!file) {
			resp.set_error(managarm::posix::Errors::NO_SUCH_FD);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			co_return {};
		}

		auto cwdResult = self->fsContext()->changeWorkingDirectory({file->associatedMount(),
				file->associatedLink()});
		if(!cwdResult) {
			resp.set_error(cwdResult.error() | toPosixProtoError);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			co_return {};
		}

		resp.set_error(managarm::posix::Errors::SUCCESS);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
		logBragiReply(resp);
	}else if(req.request_type() == managarm::posix::CntReqType::DUP) {
		logRequest(logRequests, self, "DUP", "fd={}", req.fd());

		auto file = self->fileContext()->getFile(req.fd());

		if (!file) {
			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::NO_SUCH_FD);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
			co_return {};
		}

		if(req.flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC)) {
			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
			co_return {};
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
		logRequest(logRequests, self, "TTY_NAME", "fd={}", req.fd());

		managarm::posix::SvrResponse resp;

		auto file = self->fileContext()->getFile(req.fd());
		if(!file) {
		    co_await sendErrorResponse(conversation, managarm::posix::Errors::NO_SUCH_FD);
		    co_return {};
		}

		auto ttynameResult = co_await file->ttyname();
		if(!ttynameResult) {
		    co_await sendErrorResponse(conversation, ttynameResult.error() | toPosixProtoError);
		    co_return {};
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

		logRequest(logRequests, self, "GETCWD", "path={}", path);

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
		logRequest(logRequests, self, "FD_GET_FLAGS");

		auto descriptor = self->fileContext()->getDescriptor(req.fd());
		if(!descriptor) {
			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::NO_SUCH_FD);

			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(resp);
			co_return {};
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
		logRequest(logRequests, self, "FD_SET_FLAGS");

		if(req.flags() & ~FD_CLOEXEC) {
			std::cout << "posix: FD_SET_FLAGS unknown flags: " << req.flags() << std::endl;
			co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}

		int closeOnExec = req.flags() & FD_CLOEXEC;
		if(self->fileContext()->setDescriptor(req.fd(), closeOnExec) != Error::success) {
			co_await sendErrorResponse(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		}

		managarm::posix::SvrResponse resp;
		resp.set_error(managarm::posix::Errors::SUCCESS);

		auto ser = resp.SerializeAsString();
		auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()));
		HEL_CHECK(send_resp.error());
		logBragiSerializedReply(ser);
	}else{
		std::cout << "posix: Illegal request" << std::endl;

		managarm::posix::SvrResponse resp;
		resp.set_error(managarm::posix::Errors::ILLEGAL_REQUEST);

		auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		HEL_CHECK(send_resp.error());
	}

	co_return {};
}

} // namespace requests
