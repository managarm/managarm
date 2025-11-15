#include "common.hpp"
#include "../eventfd.hpp"
#include "../inotify.hpp"
#include "../pidfd.hpp"
#include "../timerfd.hpp"
#include <sys/inotify.h>
#include <sys/pidfd.h>
#include <sys/timerfd.h>

namespace requests {

// INOTIFY_CREATE handler
async::result<void> handleInotifyCreate(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::InotifyCreateRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "INOTIFY_CREATE");

	if(req->flags() & ~(managarm::posix::OpenFlags::OF_CLOEXEC | managarm::posix::OpenFlags::OF_NONBLOCK)) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	auto file = inotify::createFile(req->flags() & managarm::posix::OpenFlags::OF_NONBLOCK);
	auto fd = ctx.self->fileContext()->attachFile(file,
			req->flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

	managarm::posix::SvrResponse resp;
	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// INOTIFY_ADD handler
async::result<void> handleInotifyAdd(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::recvBuffer(tail.data(), tail.size())
		);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::InotifyAddRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	managarm::posix::SvrResponse resp;

	logRequest(logRequests || logPaths, ctx, "INOTIFY_ADD");

	auto ifile = ctx.self->fileContext()->getFile(req->fd());
	if(!ifile) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
		co_return;
	} else if(ifile->kind() != FileKind::inotify) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	ResolveFlags flags = 0;

	if(req->flags() & IN_DONT_FOLLOW)
		flags |= resolveDontFollow;

	PathResolver resolver;
	resolver.setup(ctx.self->fsContext()->getRoot(),
			ctx.self->fsContext()->getWorkingDirectory(), req->path(), ctx.self.get());
	auto resolveResult = co_await resolver.resolve(flags);
	if(!resolveResult) {
		if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return;
		} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return;
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
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// INOTIFY_RM handler
async::result<void> handleInotifyRm(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::InotifyRmRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests || logPaths, ctx, "INOTIFY_RM");
	managarm::posix::InotifyRmReply resp;

	auto ifile = ctx.self->fileContext()->getFile(req->ifd());
	if(!ifile) {
		resp.set_error(managarm::posix::Errors::BAD_FD);
		co_return;
	} else if(ifile->kind() != FileKind::inotify) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	if(inotify::removeWatch(ifile.get(), req->wd()))
		resp.set_error(managarm::posix::Errors::SUCCESS);
	else
		resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// EVENTFD_CREATE handler
async::result<void> handleEventfdCreate(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::EventfdCreateRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "EVENTFD_CREATE");

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
		auto fd = ctx.self->fileContext()->attachFile(file,
				req->flags() & managarm::posix::EventFdFlags::CLOEXEC);

		if (fd) {
			resp.set_error(managarm::posix::Errors::SUCCESS);
			resp.set_fd(fd.value());
		} else {
			resp.set_error(fd.error() | toPosixProtoError);
		}
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// TIMER_FD_CREATE handler
async::result<void> handleTimerFdCreate(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::TimerFdCreateRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "TIMER_FD_CREATE");

	if(req->flags() & ~(TFD_CLOEXEC | TFD_NONBLOCK)) {
		std::println("posix: Unsupported flags {} for timerfd_create()", req->flags());
		co_await sendErrorResponse<managarm::posix::TimerFdCreateResponse>(ctx,
			managarm::posix::Errors::ILLEGAL_ARGUMENTS
		);
		co_return;
	}

	if (req->clock() != CLOCK_MONOTONIC && req->clock() != CLOCK_REALTIME) {
		std::println("posix: timerfd is not supported for clock {}", req->clock());
		co_await sendErrorResponse<managarm::posix::TimerFdCreateResponse>(ctx,
			managarm::posix::Errors::ILLEGAL_ARGUMENTS
		);
		co_return;
	}

	auto file = timerfd::createFile(req->clock(), req->flags() & TFD_NONBLOCK);
	auto fd = ctx.self->fileContext()->attachFile(file, req->flags() & TFD_CLOEXEC);

	managarm::posix::TimerFdCreateResponse resp;
	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
	}

	auto [sendResp] = co_await helix_ng::exchangeMsgs(ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}));
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

// TIMER_FD_SET handler
async::result<void> handleTimerFdSet(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::TimerFdSetRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "TIMER_FD_SET");

	auto file = ctx.self->fileContext()->getFile(req->fd());
	if (!file) {
		co_await sendErrorResponse<managarm::posix::TimerFdSetResponse>(ctx,
			managarm::posix::Errors::NO_SUCH_FD
		);
		co_return;
	} else if(file->kind() != FileKind::timerfd) {
		co_await sendErrorResponse<managarm::posix::TimerFdSetResponse>(ctx,
			managarm::posix::Errors::ILLEGAL_ARGUMENTS
		);
		co_return;
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

	auto [sendResp] = co_await helix_ng::exchangeMsgs(ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}));
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

// TIMER_FD_GET handler
async::result<void> handleTimerFdGet(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::TimerFdGetRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "TIMER_FD_GET");

	auto file = ctx.self->fileContext()->getFile(req->fd());
	if (!file) {
		co_await sendErrorResponse<managarm::posix::TimerFdGetResponse>(ctx,
			managarm::posix::Errors::NO_SUCH_FD
		);
		co_return;
	} else if(file->kind() != FileKind::timerfd) {
		co_await sendErrorResponse<managarm::posix::TimerFdGetResponse>(ctx,
			managarm::posix::Errors::ILLEGAL_ARGUMENTS
		);
		co_return;
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

	auto [sendResp] = co_await helix_ng::exchangeMsgs(ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}));
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

// PIDFD_OPEN handler
async::result<void> handlePidfdOpen(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::PidfdOpenRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	auto proc = Process::findProcess(req->pid());
	if(!proc) {
		co_await sendErrorResponse<managarm::posix::PidfdOpenResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	auto pidfd = createPidfdFile(proc->threadGroup()->weak_from_this(), req->flags() & PIDFD_NONBLOCK);
	auto fd = ctx.self->fileContext()->attachFile(pidfd, req->flags() & PIDFD_NONBLOCK);

	managarm::posix::PidfdOpenResponse resp;
	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// PIDFD_SEND_SIGNAL handler
async::result<void> handlePidfdSendSignal(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::PidfdSendSignalRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	auto fd = ctx.self->fileContext()->getFile(req->pidfd());
	if(!fd || fd->kind() != FileKind::pidfd) {
		co_await sendErrorResponse<managarm::posix::PidfdSendSignalResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	auto pid = smarter::static_pointer_cast<pidfd::OpenFile>(fd)->pid();
	if(pid <= 0) {
		co_await sendErrorResponse<managarm::posix::PidfdSendSignalResponse>(ctx, managarm::posix::Errors::NO_SUCH_RESOURCE);
		co_return;
	}

	auto target = Process::findProcess(pid);
	if(!target) {
		co_await sendErrorResponse<managarm::posix::PidfdSendSignalResponse>(ctx, managarm::posix::Errors::NO_SUCH_RESOURCE);
		co_return;
	}

	UserSignal info;
	info.pid = ctx.self->pid();
	info.uid = 0;
	target->threadGroup()->signalContext()->issueSignal(req->signal(), info);

	managarm::posix::PidfdSendSignalResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// PIDFD_GET_PID handler
async::result<void> handlePidfdGetPid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::PidfdGetPidRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	auto fd = ctx.self->fileContext()->getFile(req->pidfd());
	if(!fd || fd->kind() != FileKind::pidfd) {
		co_await sendErrorResponse<managarm::posix::PidfdGetPidResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	auto pid = smarter::static_pointer_cast<pidfd::OpenFile>(fd)->pid();
	if(pid <= 0) {
		co_await sendErrorResponse<managarm::posix::PidfdGetPidResponse>(ctx, managarm::posix::Errors::NO_SUCH_RESOURCE);
		co_return;
	}

	managarm::posix::PidfdGetPidResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(pid);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

} // namespace requests
