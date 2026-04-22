#include "common.hpp"
#include <iostream>

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetPidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_PID", "pid={}", self->pid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(self->pid());

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetPpidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_PPID", "ppid={}", self->getParent()->pid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_pid(self->getParent()->pid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetUidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_UID", "uid={}", self->threadGroup()->uid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_uid(self->threadGroup()->uid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetUidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_UID", "uid={}", req.uid());

	Error err = self->threadGroup()->setUid(req.uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::SUCCESS);
	}
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetEuidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_EUID", "euid={}", self->threadGroup()->euid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_uid(self->threadGroup()->euid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetEuidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_EUID", "euid={}", req.uid());

	Error err = self->threadGroup()->setEuid(req.uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::SUCCESS);
	}
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetGidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_GID", "gid={}", self->threadGroup()->gid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_uid(self->threadGroup()->gid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetEgidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_EGID", "egid={}", self->threadGroup()->egid());

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_uid(self->threadGroup()->egid());

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetGidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_GID");

	Error err = self->threadGroup()->setGid(req.uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::SUCCESS);
	}
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetEgidRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_EGID");

	Error err = self->threadGroup()->setEgid(req.uid());
	if(err == Error::accessDenied) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ACCESS_DENIED);
	} else if(err == Error::illegalArguments) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
	} else {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::SUCCESS);
	}
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::GetGroupsRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "GET_GROUPS");

	const auto &groups = self->threadGroup()->supplementaryGroups();
	size_t sendEntries = groups.size();

	managarm::posix::GetGroupsResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	if (req.size()) {
		if (groups.size() > req.size()) {
			co_await sendErrorResponse<managarm::posix::GetGroupsResponse>(conversation,
					managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return {};
		}

		resp.set_entries(groups.size());
	} else {
		resp.set_entries(groups.size());
		sendEntries = 0;
	}

	auto [send_resp, send_list] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
		helix_ng::sendBuffer(groups.data(), sizeof(gid_t) * sendEntries)
	);
	HEL_CHECK(send_resp.error());
	HEL_CHECK(send_list.error());

	logBragiReply(resp);
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::SetGroupsRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "SET_GROUPS");

	std::vector<gid_t> list;
	list.resize(req.entries());

	auto [recv_list] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(list.data(), req.entries() * sizeof(gid_t))
		);
	HEL_CHECK(recv_list.error());

	auto err = self->threadGroup()->setSupplementaryGroups(std::move(list));

	managarm::posix::SetGroupsResponse resp;
	resp.set_error(err | toPosixProtoError);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(resp);
	co_return {};
}

} // namespace requests
