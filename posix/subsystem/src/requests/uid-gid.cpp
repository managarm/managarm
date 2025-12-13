#include "common.hpp"
#include <iostream>

namespace requests {

// GET_PID handler
async::result<void> handleGetPid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::GetPidRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "GET_PID", "pid={}", ctx.self->pid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(ctx.self->pid());

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

// GET_PPID handler
async::result<void> handleGetPpid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::GetPpidRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "GET_PPID", "ppid={}", ctx.self->getParent()->pid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(ctx.self->getParent()->pid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// GET_UID handler
async::result<void> handleGetUid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::GetUidRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "GET_UID", "uid={}", ctx.self->threadGroup()->uid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_uid(ctx.self->threadGroup()->uid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// SET_UID handler
async::result<void> handleSetUid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::SetUidRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SET_UID", "uid={}", req->uid());

	Error err = ctx.self->threadGroup()->setUid(req->uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
	}
}

// GET_EUID handler
async::result<void> handleGetEuid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::GetEuidRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "GET_EUID", "euid={}", ctx.self->threadGroup()->euid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_uid(ctx.self->threadGroup()->euid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// SET_EUID handler
async::result<void> handleSetEuid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::SetEuidRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SET_EUID", "euid={}", req->uid());

	Error err = ctx.self->threadGroup()->setEuid(req->uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
	}
}

// GET_GID handler
async::result<void> handleGetGid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::GetGidRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "GET_GID", "gid={}", ctx.self->threadGroup()->gid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_uid(ctx.self->threadGroup()->gid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// GET_EGID handler
async::result<void> handleGetEgid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::GetEgidRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "GET_EGID", "egid={}", ctx.self->threadGroup()->egid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_uid(ctx.self->threadGroup()->egid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// SET_GID handler
async::result<void> handleSetGid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::SetGidRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SET_GID");

	Error err = ctx.self->threadGroup()->setGid(req->uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
	}
}

// SET_EGID handler
async::result<void> handleSetEgid(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::SetEgidRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SET_EGID");

	Error err = ctx.self->threadGroup()->setEgid(req->uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
	}
}

// GET_GROUPS handler
async::result<void> handleGetGroups(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::GetGroupsRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "GET_GROUPS");

	const auto &groups = ctx.self->threadGroup()->supplementaryGroups();
	size_t sendEntries = groups.size();

	managarm::posix::GetGroupsResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	if (req->size()) {
		if (groups.size() > req->size()) {
			co_await sendErrorResponse<managarm::posix::GetGroupsResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return;
		}

		resp.set_entries(groups.size());
	} else {
		resp.set_entries(groups.size());
		sendEntries = 0;
	}

	auto [send_resp, send_list] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
		helix_ng::sendBuffer(groups.data(), sizeof(gid_t) * sendEntries)
	);
	HEL_CHECK(send_resp.error());
	HEL_CHECK(send_list.error());

	logBragiReply(ctx, resp);
}

// SET_GROUPS handler
async::result<void> handleSetGroups(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::SetGroupsRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SET_GROUPS");

	std::vector<gid_t> list;
	list.resize(req->entries());

	auto [recv_list] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::recvBuffer(list.data(), req->entries() * sizeof(gid_t))
		);
	HEL_CHECK(recv_list.error());

	auto err = ctx.self->threadGroup()->setSupplementaryGroups(std::move(list));

	managarm::posix::SetGroupsResponse resp;
	resp.set_error(err | toPosixProtoError);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

}
