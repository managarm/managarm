#include "common.hpp"
#include <sys/wait.h>
#include <sys/resource.h>
#include <iostream>

namespace requests {

// WAIT_ID handler
async::result<void> handleWaitId(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::WaitIdRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	if((req->flags() & ~(WNOHANG | WCONTINUED | WEXITED | WSTOPPED | WNOWAIT)) ||
		!(req->flags() & (WEXITED /*| WSTOPPED | WCONTINUED*/))) {
		std::cout << "posix: WAIT_ID invalid flags: " << req->flags() << std::endl;
		co_await sendErrorResponse<managarm::posix::WaitIdResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
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
		auto fd = ctx.self->fileContext()->getFile(req->id());
		if(!fd || fd->kind() != FileKind::pidfd) {
			co_await sendErrorResponse<managarm::posix::WaitIdResponse>(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}
		auto pidfd = smarter::static_pointer_cast<pidfd::OpenFile>(fd);
		wait_pid = pidfd->pid();
		if(pidfd->nonBlock())
			flags |= waitNonBlocking;
	} else {
		std::cout << "\e[31mposix: WAIT_ID idtype other than P_PID, P_PIDFD and P_ALL are not implemented\e[39m" << std::endl;
		co_await sendErrorResponse<managarm::posix::WaitIdResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	logRequest(logRequests, ctx, "WAIT_ID", "pid={}", wait_pid);

	auto wait_result = co_await ctx.self->wait(wait_pid, flags, {});

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
			resp.set_sig_code(ctx.self->threadGroup()->getDumpable() ? CLD_DUMPED : CLD_KILLED);
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
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

// SET_AFFINITY handler
async::result<void> handleSetAffinity(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::SetAffinityRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SET_AFFINITY");

	auto handle = ctx.self->threadDescriptor().getHandle();

	if(ctx.self->pid() != req->pid()) {
		// TODO: permission checking
		auto target_process = ctx.self->findProcess(req->pid());
		if(target_process == nullptr) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return;
		}
		handle = target_process->threadDescriptor().getHandle();
	}

	HelError e = helSetAffinity(handle, req->mask().data(), req->mask().size());

	if(e == kHelErrIllegalArgs) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	} else if(e != kHelErrNone) {
		std::cout << "posix: SET_AFFINITY hel call returned unexpected error: " << e << std::endl;
		co_await sendErrorResponse(ctx, managarm::posix::Errors::INTERNAL_ERROR);
		co_return;
	}

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

// GET_AFFINITY handler
async::result<void> handleGetAffinity(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::GetAffinityRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "GET_AFFINITY");

	if(!req->size()) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	std::vector<uint8_t> affinity(req->size());

	auto handle = ctx.self->threadDescriptor().getHandle();

	if(req->pid() && ctx.self->pid() != req->pid()) {
		// TODO: permission checking
		auto target_process = ctx.self->findProcess(req->pid());
		if(target_process == nullptr) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return;
		}
		handle = target_process->threadDescriptor().getHandle();
	}

	size_t actual_size;
	HelError e = helGetAffinity(handle, affinity.data(), req->size(), &actual_size);

	if(e == kHelErrBufferTooSmall) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	} else if(e != kHelErrNone) {
		std::cout << "posix: GET_AFFINITY hel call returned unexpected error: " << e << std::endl;
		co_await sendErrorResponse(ctx, managarm::posix::Errors::INTERNAL_ERROR);
		co_return;
	}

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(ctx.self->pid());

	auto [sendResp, sendData] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
		helix_ng::sendBuffer(affinity.data(), affinity.size())
	);
	HEL_CHECK(sendResp.error());
	HEL_CHECK(sendData.error());
	logBragiReply(ctx, resp);
}

// GET_PGID handler
async::result<void> handleGetPgid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::GetPgidRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "GET_PGID");

	std::shared_ptr<Process> target;
	if(req->pid()) {
		target = Process::findProcess(req->pid());
		if(!target) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_RESOURCE);
			co_return;
		}
	} else {
		target = ctx.self;
	}

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(target->pgPointer()->getHull()->getPid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// SET_PGID handler
async::result<void> handleSetPgid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::SetPgidRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SET_PGID");

	std::shared_ptr<Process> target;

	if (req->pgid() < 0) {
		// POSIX: reject negative `pgid` (or implementation-unsupported) values with EINVAL
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	} else if (req->pid() > 0) {
		target = Process::findProcess(req->pid());
		if (!target) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_RESOURCE);
			co_return;
		}

		auto isSelf = req->pid() == ctx.self->pid();
		auto isChild = (!isSelf && target->getParent() && target->getParent()->pid() == ctx.self->pid());

		// POSIX: if `pid` is not the PID of the calling process or its children, ESRCH.
		if (!isSelf && !isChild) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_RESOURCE);
			co_return;
		}

		// POSIX: if target process is not in the same session, EPERM.
		if (target->pgPointer()->getSession() != ctx.self->pgPointer()->getSession()) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::INSUFFICIENT_PERMISSION);
			co_return;
		}

		// POSIX: if `pid` matches the process ID of a child and the child has successfully
		// executed one of the `exec*` functions, return EACCES.
		if (isChild && target->didExecute()) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ACCESS_DENIED);
			co_return;
		}
	} else {
		target = ctx.self;
	}

	// POSIX: We can't change the process group ID of the session leader, EPERM
	if (target->pid() == target->pgPointer()->getSession()->getSessionId()) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::INSUFFICIENT_PERMISSION);
		co_return;
	}

	// If pgid is 0, we're going to set it to the calling process's pid, use the target group.
	auto resolvedPgid = req->pgid() ? req->pgid() : target->pid();
	std::shared_ptr<ProcessGroup> group = target->pgPointer()->getSession()->getProcessGroupById(resolvedPgid);

	if(group) {
		// Found, do permission checking and join
		group->reassociateProcess(target->threadGroup());
	} else {
		// Not found, making it if pgid and pid match, or if pgid is 0, indicating that we should make one
		if(target->pid() == req->pgid() || !req->pgid()) {
			target->pgPointer()->getSession()->spawnProcessGroup(target->threadGroup());
		} else {
			// POSIX: invalid `pgid` supplied, return EINVAL.
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return;
		}
	}

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// GET_SID handler
async::result<void> handleGetSid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::GetSidRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "GET_SID", "pid={}", req->pid());

	std::shared_ptr<Process> target;
	if(req->pid()) {
		target = Process::findProcess(req->pid());
		if(!target) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_RESOURCE);
			co_return;
		}
	} else {
		target = ctx.self;
	}
	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(target->pgPointer()->getSession()->getSessionId());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// PARENT_DEATH_SIGNAL handler
async::result<void> handleParentDeathSignal(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::ParentDeathSignalRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	ctx.self->threadGroup()->setParentDeathSignal(req->signal() ? std::optional{req->signal()} : std::nullopt);

	managarm::posix::ParentDeathSignalResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// PROCESS_DUMPABLE handler
async::result<void> handleProcessDumpable(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::ProcessDumpableRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	managarm::posix::ProcessDumpableResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	if(req->set()) {
		ctx.self->threadGroup()->setDumpable(req->new_value());
	}

	resp.set_value(ctx.self->threadGroup()->getDumpable());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// SET_RESOURCE_LIMIT handler
async::result<void> handleSetResourceLimit(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::SetResourceLimitRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	managarm::posix::SetResourceLimitResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	switch(req->resource()) {
		case RLIMIT_NOFILE:
			ctx.self->fileContext()->setFdLimit(req->max());
			break;
		default:
			resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			break;
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

} // namespace requests
