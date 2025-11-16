#include "common.hpp"
#include <fcntl.h>

namespace requests {

// DUP2 handler
async::result<void> handleDup2(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::Dup2Request>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}
	logRequest(logRequests, ctx, "DUP2", "fd={}", req->fd());

	auto file = ctx.self->fileContext()->getFile(req->fd());

	managarm::posix::Dup2Response resp;

	if (!file || req->newfd() < 0) {
		resp.set_error(managarm::posix::Errors::NO_SUCH_FD);
		auto [send_resp] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

		HEL_CHECK(send_resp.error());
		logBragiReply(ctx, resp);
		co_return;
	}

	if(req->flags()) {
		if(!(req->flags() & O_CLOEXEC)) {
				resp.set_error(managarm::posix::Errors::ILLEGAL_ARGUMENTS);

				auto [send_resp] = co_await helix_ng::exchangeMsgs(
					ctx.conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);

				HEL_CHECK(send_resp.error());
				co_return;
		}
	}
	bool closeOnExec = (req->flags() & O_CLOEXEC);

	std::expected<int, Error> result = req->newfd();
	if(req->fcntl_mode())
		result = ctx.self->fileContext()->attachFile(file, closeOnExec, req->newfd());
	else
		result = ctx.self->fileContext()->attachFile(req->newfd(), file, closeOnExec)
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
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// IS_TTY handler
async::result<void> handleIsTty(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::IsTtyRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}
	logRequest(logRequests, ctx, "IS_TTY", "fd={}", req->fd());

	auto file = ctx.self->fileContext()->getFile(req->fd());
	if(!file) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
		co_return;
	}

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_mode(file->isTerminal());

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

// FIOCLEX (ioctl set close-on-exec) handler
async::result<void> handleIoctlFioclex(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::IoctlFioclexRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "FIOCLEX");

	if(ctx.self->fileContext()->setDescriptor(req->fd(), true) != Error::success) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
		co_return;
	}

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto ser = resp.SerializeAsString();
	auto [send_resp] = co_await helix_ng::exchangeMsgs(ctx.conversation,
			helix_ng::sendBuffer(ser.data(), ser.size()));
	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// CLOSE handler
async::result<void> handleClose(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::CloseRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "CLOSE", "fd={}", req->fd());

	auto closeErr = ctx.self->fileContext()->closeFile(req->fd());

	if(closeErr != Error::success) {
		if(closeErr == Error::noSuchFile) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		} else {
			std::cout << "posix: Unhandled error returned from closeFile" << std::endl;
			co_return;
		}
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

} // namespace requests
