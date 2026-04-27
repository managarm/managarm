#include "common.hpp"
#include <fcntl.h>

namespace requests {

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::Dup2Request &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "DUP2", "fd={}", req.fd());

	auto file = self->fileContext()->getFile(req.fd());

	managarm::posix::Dup2Response resp;

	if (!file || req.newfd() < 0) {
		resp.set_error(managarm::posix::Errors::NO_SUCH_FD);
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

		HEL_CHECK(send_resp.error());
		logBragiReply(resp);
		co_return {};
	}

	if(req.flags()) {
		if(!(req.flags() & O_CLOEXEC)) {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

				HEL_CHECK(send_resp.error());
				co_return {};
		}
	}
	bool closeOnExec = (req.flags() & O_CLOEXEC);

	std::expected<int, Error> result = req.newfd();
	if(req.fcntl_mode())
		result = self->fileContext()->attachFile(file, closeOnExec, req.newfd());
	else
		result = self->fileContext()->attachFile(req.newfd(), file, closeOnExec)
			.transform([&]() {
				return req.newfd();
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
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::IsTtyRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "IS_TTY", "fd={}", req.fd());

	auto file = self->fileContext()->getFile(req.fd());
	if(!file) {
		co_await sendErrorResponse(conversation, managarm::posix::Errors::NO_SUCH_FD);
		co_return {};
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
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::IoctlFioclexRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "FIOCLEX");

	if(self->fileContext()->setDescriptor(req.fd(), true) != Error::success) {
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
	co_return {};
}

async::result<std::expected<void, DispatchError>>
HandleRequest::operator()(managarm::posix::CloseRequest &&req,
		helix::BorrowedDescriptor conversation, bragi::preamble preamble,
		std::shared_ptr<Process> self, std::shared_ptr<Generation>) {
	id = preamble.id();
	logBragiRequest(req);

	logRequest(logRequests, self, "CLOSE", "fd={}", req.fd());

	auto closeErr = self->fileContext()->closeFile(req.fd());

	if(closeErr != Error::success) {
		if(closeErr == Error::noSuchFile) {
			co_await sendErrorResponse(conversation, managarm::posix::Errors::NO_SUCH_FD);
			co_return {};
		} else {
			std::cout << "posix: Unhandled error returned from closeFile" << std::endl;
			co_return {};
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
	co_return {};
}

} // namespace requests
