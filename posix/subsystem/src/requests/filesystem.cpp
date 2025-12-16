#include "common.hpp"
#include "../vfs.hpp"
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <iostream>

namespace requests {

// Forward declarations for helper functions if needed
// (These would need to be extracted from the original file or made accessible)

// CHROOT handler
async::result<void> handleChroot(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::ChrootRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "CHROOT");

	auto pathResult = co_await resolve(ctx.self->fsContext()->getRoot(),
			ctx.self->fsContext()->getWorkingDirectory(), req->path(), ctx.self.get());
	if(!pathResult) {
		if(pathResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse<managarm::posix::ChrootResponse>(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return;
		} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse<managarm::posix::ChrootResponse>(ctx, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return;
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return;
		}
	}
	auto path = pathResult.value();
	ctx.self->fsContext()->changeRoot(path);

	managarm::posix::ChrootResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// CHDIR handler
async::result<void> handleChdir(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::ChdirRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "CHDIR");

	auto pathResult = co_await resolve(ctx.self->fsContext()->getRoot(),
			ctx.self->fsContext()->getWorkingDirectory(), req->path(), ctx.self.get());
	if(!pathResult) {
		if(pathResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse<managarm::posix::ChdirResponse>(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return;
		} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse<managarm::posix::ChdirResponse>(ctx, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return;
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return;
		}
	}
	auto path = pathResult.value();
	ctx.self->fsContext()->changeWorkingDirectory(path);

	managarm::posix::ChdirResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// ACCESSAT handler
async::result<void> handleAccessAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::AccessAtRequest>(ctx.recv_head, tail);

	if(!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
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
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return;
		}
	}

	if(req->fd() == AT_FDCWD) {
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());

		if(!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	auto pathResult = co_await resolve(ctx.self->fsContext()->getRoot(),
			relative_to, req->path(), ctx.self.get(), resolveFlags);
	if(!pathResult) {
		if(pathResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return;
		} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return;
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return;
		}
	}

	logRequest(logRequests || logPaths, ctx, "ACCESSAT", "'{}'", pathResult.value().getPath(ctx.self->fsContext()->getRoot()));

	co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
}

// MKDIRAT handler
async::result<void> handleMkdirAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::MkdirAtRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests || logPaths, ctx, "MKDIRAT", "path='{}'", req->path());

	if(!req->path().size()) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;

	if(req->fd() == AT_FDCWD) {
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());

		if (!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(ctx.self->fsContext()->getRoot(),
			relative_to, req->path(), ctx.self.get());
	auto resolveResult = co_await resolver.resolve(resolvePrefix);
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

	if(!resolver.hasComponent()) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ALREADY_EXISTS);
		co_return;
	}

	auto parent = resolver.currentLink()->getTarget();
	auto existsResult = co_await parent->getLink(resolver.nextComponent());
	if (existsResult) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ALREADY_EXISTS);
		co_return;
	}

	auto result = co_await parent->mkdir(resolver.nextComponent());

	if(auto error = std::get_if<Error>(&result); error) {
		assert(*error == Error::illegalOperationTarget);
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	auto target = std::get<std::shared_ptr<FsLink>>(result)->getTarget();
	auto chmodResult = co_await target->chmod(req->mode() & ~ctx.self->fsContext()->getUmask() & 0777);
	if (chmodResult != Error::success) {
		std::cout << "posix: chmod failed when creating directory for MkdirAtRequest!" << std::endl;
		co_await sendErrorResponse(ctx, managarm::posix::Errors::INTERNAL_ERROR);
		co_return;
	}

	co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
}

// MKFIFOAT handler
async::result<void> handleMkfifoAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::MkfifoAtRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests || logPaths, ctx, "MKFIFOAT", "path='{}'", req->path());

	if (!req->path().size()) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> target_link;

	if (req->fd() == AT_FDCWD) {
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());

		if (!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(ctx.self->fsContext()->getRoot(),
			relative_to, req->path(), ctx.self.get());
	auto resolveResult = co_await resolver.resolve(resolvePrefix | resolveNoTrailingSlash);
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

	auto parent = resolver.currentLink()->getTarget();
	if(co_await parent->getLink(resolver.nextComponent())) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ALREADY_EXISTS);
		co_return;
	}

	auto result = co_await parent->mkfifo(
		resolver.nextComponent(),
		req->mode() & ~ctx.self->fsContext()->getUmask()
	);
	if(!result) {
		std::cout << "posix: Unexpected failure from mkfifo()" << std::endl;
		co_return;
	}

	co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
}

// LINKAT handler
async::result<void> handleLinkAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::LinkAtRequest>(ctx.recv_head, tail);

	logRequest(logRequests, ctx, "LINKAT");

	if(req->flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_FOLLOW)) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
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
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());

		if(!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(ctx.self->fsContext()->getRoot(),
			relative_to, req->path(), ctx.self.get());
	auto resolveResult = co_await resolver.resolve();
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

	if (req->newfd() == AT_FDCWD) {
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->newfd());

		if(!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver new_resolver;
	new_resolver.setup(ctx.self->fsContext()->getRoot(),
			relative_to, req->target_path(), ctx.self.get());
	auto new_resolveResult = co_await new_resolver.resolve(
			resolvePrefix | resolveNoTrailingSlash);
	if(!new_resolveResult) {
		if(new_resolveResult.error() == protocols::fs::Error::illegalOperationTarget) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_OPERATION_TARGET);
			co_return;
		} else if(new_resolveResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return;
		} else if(new_resolveResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return;
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

	co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
}

// SYMLINKAT handler
async::result<void> handleSymlinkAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::SymlinkAtRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	ViewPath relativeTo;
	smarter::shared_ptr<File, FileHandle> file;

	if (!req->path().size()) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	if(req->fd() == AT_FDCWD) {
		relativeTo = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());
		if (!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relativeTo = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(ctx.self->fsContext()->getRoot(),
			relativeTo, req->path(), ctx.self.get());
	auto resolveResult = co_await resolver.resolve(
			resolvePrefix | resolveNoTrailingSlash);
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

	logRequest(logRequests || logPaths, ctx, "SYMLINK", "'{}{}' -> '{}'",
		ViewPath{resolver.currentView(), resolver.currentLink()}
			.getPath(ctx.self->fsContext()->getRoot()),
		resolver.nextComponent(),
		req->target_path());

	auto parent = resolver.currentLink()->getTarget();
	auto result = co_await parent->symlink(resolver.nextComponent(), req->target_path());
	if(auto error = std::get_if<Error>(&result); error) {
		if(*error == Error::alreadyExists) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ALREADY_EXISTS);
			co_return;
		} else {
			assert(*error == Error::illegalOperationTarget);
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
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

// READLINKAT handler
async::result<void> handleReadlinkAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::ReadlinkAtRequest>(ctx.recv_head, tail);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;

	if(req->fd() == AT_FDCWD) {
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());

		if (!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	auto pathResult = co_await resolve(ctx.self->fsContext()->getRoot(),
			relative_to, req->path(), ctx.self.get(), resolveDontFollow);
	if(!pathResult) {
		if(pathResult.error() == protocols::fs::Error::fileNotFound) {
			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::FILE_NOT_FOUND);

			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(ctx.conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(nullptr, 0)
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(ctx, resp);
			co_return;
		} else if(pathResult.error() == protocols::fs::Error::notDirectory) {
			managarm::posix::SvrResponse resp;
			resp.set_error(managarm::posix::Errors::NOT_A_DIRECTORY);

			auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(ctx.conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(nullptr, 0)
			);
			HEL_CHECK(send_resp.error());
			logBragiReply(ctx, resp);
			co_return;
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return;
		}
	}
	auto path = pathResult.value();

	auto result = co_await path.second->getTarget()->readSymlink(path.second.get(), ctx.self.get());
	if(auto error = std::get_if<Error>(&result); error) {
		managarm::posix::SvrResponse resp;
		resp.set_error(*error | toPosixProtoError);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(nullptr, 0)
		);
		HEL_CHECK(send_resp.error());
		logBragiReply(ctx, resp);
	}else{
		auto &target = std::get<std::string>(result);

		logRequest(logRequests || logPaths, ctx, "READLINKAT", "'{}' -> '{}'",
			path.getPath(ctx.self->fsContext()->getRoot()),
			target);

		managarm::posix::SvrResponse resp;
		resp.set_error(managarm::posix::Errors::SUCCESS);

		auto [send_resp, send_data] = co_await helix_ng::exchangeMsgs(ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::sendBuffer(target.data(), target.size())
		);
		HEL_CHECK(send_resp.error());
		logBragiReply(ctx, resp);
	}
}

// RENAMEAT handler
async::result<void> handleRenameAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::RenameAtRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;

	if (req->fd() == AT_FDCWD) {
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());

		if (!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(ctx.self->fsContext()->getRoot(),
			relative_to, req->path(), ctx.self.get());
	auto resolveResult = co_await resolver.resolve(resolveDontFollow);
	if(!resolveResult) {
		if(resolveResult.error() == protocols::fs::Error::isDirectory) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::IS_DIRECTORY);
			co_return;
		} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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

	if (req->newfd() == AT_FDCWD) {
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->newfd());

		if (!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	// TODO: Add resolveNoTrailingSlash if source is not a directory?
	PathResolver new_resolver;
	new_resolver.setup(ctx.self->fsContext()->getRoot(),
			relative_to, req->target_path(), ctx.self.get());
	auto new_resolveResult = co_await new_resolver.resolve(resolvePrefix);
	if(!new_resolveResult) {
		if(new_resolveResult.error() == protocols::fs::Error::isDirectory) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::IS_DIRECTORY);
			co_return;
		} else if(new_resolveResult.error() == protocols::fs::Error::fileNotFound) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return;
		} else if(new_resolveResult.error() == protocols::fs::Error::notDirectory) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NOT_A_DIRECTORY);
			co_return;
		} else {
			std::cout << "posix: Unexpected failure from resolve()" << std::endl;
			co_return;
		}
	}

	logRequest(logRequests || logPaths, ctx, "RENAMEAT", "'{}' -> '{}{}'",
		ViewPath(resolver.currentView(), resolver.currentLink())
		.getPath(ctx.self->fsContext()->getRoot()),
		ViewPath(new_resolver.currentView(), new_resolver.currentLink())
		.getPath(ctx.self->fsContext()->getRoot()),
		new_resolver.nextComponent());

	auto superblock = resolver.currentLink()->getTarget()->superblock();
	auto directory = new_resolver.currentLink()->getTarget();
	assert(superblock == directory->superblock());
	auto result = co_await superblock->rename(resolver.currentLink().get(),
			directory.get(), new_resolver.nextComponent());
	if(!result) {
		assert(result.error() == Error::alreadyExists);
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ALREADY_EXISTS);
		co_return;
	}

	co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
}

// UNLINKAT handler
async::result<void> handleUnlinkAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::UnlinkAtRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> target_link;

	if(req->flags()) {
		if(req->flags() & AT_REMOVEDIR) {
			std::cout << "posix: UNLINKAT flag AT_REMOVEDIR handling unimplemented" << std::endl;
		} else {
			std::cout << "posix: UNLINKAT flag handling unimplemented with unknown flag: " << req->flags() << std::endl;
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		}
	}

	if(req->fd() == AT_FDCWD) {
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());

		if (!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(ctx.self->fsContext()->getRoot(),
			relative_to, req->path(), ctx.self.get());

	auto resolveResult = co_await resolver.resolve(resolveDontFollow);
	if(!resolveResult) {
		if(resolveResult.error() == protocols::fs::Error::isDirectory) {
			// TODO: Only when AT_REMOVEDIR is not specified, fix this when flag handling is implemented.
			co_await sendErrorResponse(ctx, managarm::posix::Errors::IS_DIRECTORY);
			co_return;
		} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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

	logRequest(logRequests || logPaths, ctx, "UNLINKAT", "path='{}'",
		ViewPath(resolver.currentView(), resolver.currentLink())
		.getPath(ctx.self->fsContext()->getRoot()));

	target_link = resolver.currentLink();

	auto owner = target_link->getOwner();
	if(!owner) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::RESOURCE_IN_USE);
		co_return;
	}
	auto result = co_await owner->unlink(target_link->getName());
	if(!result) {
		if(result.error() == Error::noSuchFile) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return;
		}else if(result.error() == Error::directoryNotEmpty) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::DIRECTORY_NOT_EMPTY);
			co_return;
		}else{
			std::cout << "posix: Unexpected failure from unlink()" << std::endl;
			co_return;
		}
	}

	co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
}

// RMDIR handler
async::result<void> handleRmdir(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::RmdirRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	std::shared_ptr<FsLink> target_link;

	PathResolver resolver;
	resolver.setup(ctx.self->fsContext()->getRoot(), ctx.self->fsContext()->getWorkingDirectory(),
			req->path(), ctx.self.get());

	auto resolveResult = co_await resolver.resolve();
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

	logRequest(logRequests || logPaths, ctx, "RMDIR", "path='{}'",
		ViewPath(resolver.currentView(), resolver.currentLink())
		.getPath(ctx.self->fsContext()->getRoot()));

	target_link = resolver.currentLink();

	auto owner = target_link->getOwner();
	auto result = co_await owner->rmdir(target_link->getName());
	if(!result) {
		co_await sendErrorResponse(ctx, result.error() | toPosixProtoError);
		co_return;
	}

	co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
}

// FSTATAT handler
async::result<void> handleFstatAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::FstatAtRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "FSTATAT");

	if (req->flags() & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH | AT_NO_AUTOMOUNT)) {
		std::cout << std::format("posix: unsupported flags {:#x} given to FSTATAT request", req->flags()) << std::endl;
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> target_link;
	std::shared_ptr<MountView> target_mount;

	if (req->fd() == AT_FDCWD) {
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());

		if (!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	if (req->flags() & AT_EMPTY_PATH) {
		target_link = file->associatedLink();
	} else {
		PathResolver resolver;
		resolver.setup(ctx.self->fsContext()->getRoot(),
				relative_to, req->path(), ctx.self.get());

		ResolveFlags resolveFlags = 0;
		if (req->flags() & AT_SYMLINK_NOFOLLOW)
		    resolveFlags |= resolveDontFollow;

		auto resolveResult = co_await resolver.resolve(resolveFlags);
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

		target_mount = resolver.currentView();
		target_link = resolver.currentLink();
	}

	// This catches cases where associatedLink is called on a file, but the file doesn't implement that.
	// Instead of blowing up, return ENOENT.
	if(target_link == nullptr) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
		co_return;
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
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// FSTATFS handler
async::result<void> handleFstatfs(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recvTail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recvTail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::FstatfsRequest>(ctx.recv_head, tail);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "FSTATFS");

	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> target_link;
	managarm::posix::FstatfsResponse resp;

	if(req->fd() >= 0) {
		file = ctx.self->fileContext()->getFile(req->fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::FstatfsResponse>(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		target_link = file->associatedLink();

		// This catches cases where associatedLink is called on a file, but the file doesn't implement that.
		// Instead of blowing up, return ENOENT.
		// TODO: fstatfs can't return ENOENT, verify this is needed
		if(target_link == nullptr) {
			co_await sendErrorResponse<managarm::posix::FstatfsResponse>(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
			co_return;
		}

		auto fsstatsResult = co_await target_link->getTarget()->superblock()->getFsstats();
		if(!fsstatsResult) {
			co_await sendErrorResponse<managarm::posix::FstatfsResponse>(ctx, fsstatsResult.error() | toPosixProtoError);
			co_return;
		}
		auto fsstats = fsstatsResult.value();

		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fstype(fsstats.f_type);
	} else {
		PathResolver resolver;
		resolver.setup(ctx.self->fsContext()->getRoot(), ctx.self->fsContext()->getWorkingDirectory(),
				req->path(), ctx.self.get());
		auto resolveResult = co_await resolver.resolve();
		if(!resolveResult) {
			if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
				co_await sendErrorResponse<managarm::posix::FstatfsResponse>(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
				co_return;
			} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
				co_await sendErrorResponse<managarm::posix::FstatfsResponse>(ctx, managarm::posix::Errors::NOT_A_DIRECTORY);
				co_return;
			} else {
				std::cout << "posix: Unexpected failure from resolve()" << std::endl;
				co_return;
			}
		}

		target_link = resolver.currentLink();
		auto fsstatsResult = co_await target_link->getTarget()->superblock()->getFsstats();
		if(!fsstatsResult) {
			co_await sendErrorResponse<managarm::posix::FstatfsResponse>(ctx, fsstatsResult.error() | toPosixProtoError);
			co_return;
		}
		auto fsstats = fsstatsResult.value();

		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fstype(fsstats.f_type);
	}

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// FCHMODAT handler
async::result<void> handleFchmodAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::FchmodAtRequest>(ctx.recv_head, tail);

	logRequest(logRequests, ctx, "FCHMODAT");

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> target_link;

	if(req->fd() == AT_FDCWD) {
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());

		if (!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	if(req->flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
		std::println("posix: unexpected flags {:#x} in fchmodat request", req->flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW));
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	if(req->flags() & AT_EMPTY_PATH) {
		target_link = file->associatedLink();
	} else {
		PathResolver resolver;
		resolver.setup(ctx.self->fsContext()->getRoot(),
			relative_to, req->path(), ctx.self.get());

		ResolveFlags resolveFlags = 0;
		if(req->flags() & AT_SYMLINK_NOFOLLOW)
			resolveFlags |= resolveDontFollow;

		auto resolveResult = co_await resolver.resolve(resolveFlags);

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

		target_link = resolver.currentLink();
	}

	co_await target_link->getTarget()->chmod(req->mode());

	co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
}

// FCHOWNAT handler
async::result<void> handleFchownAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::FchownAtRequest>(ctx.recv_head, tail);

	logRequest(logRequests, ctx, "FCHOWNAT");

	ViewPath relativeTo;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsLink> targetLink;

	if(req->fd() == AT_FDCWD) {
		relativeTo = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());

		if (!file) {
			co_await sendErrorResponse<managarm::posix::FchownAtResponse>(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relativeTo = {file->associatedMount(), file->associatedLink()};
	}

	if(req->flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) {
		std::println("posix: unexpected flags {:#x} in fchownat request", req->flags() & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW));
		co_await sendErrorResponse<managarm::posix::FchownAtResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	if(req->flags() & AT_EMPTY_PATH) {
		targetLink = file->associatedLink();
	} else {
		PathResolver resolver;
		resolver.setup(ctx.self->fsContext()->getRoot(),
			relativeTo, req->path(), ctx.self.get());

		ResolveFlags resolveFlags = 0;
		if(req->flags() & AT_SYMLINK_NOFOLLOW)
			resolveFlags |= resolveDontFollow;

		auto resolveResult = co_await resolver.resolve(resolveFlags);

		if(!resolveResult) {
			co_await sendErrorResponse<managarm::posix::FchownAtResponse>(ctx, resolveResult.error() | toPosixError | toPosixProtoError);
			co_return;
		}

		targetLink = resolver.currentLink();
	}

	std::optional<uid_t> uid;
	std::optional<gid_t> gid;
	if(req->uid() != -1)
		uid = req->uid();
	if(req->gid() != -1)
		gid = req->gid();

	auto result = co_await targetLink->getTarget()->chown(uid, gid);
	if(!result) {
		co_await sendErrorResponse<managarm::posix::FchownAtResponse>(ctx, result.error() | toPosixProtoError);
		co_return;
	}

	co_await sendErrorResponse<managarm::posix::FchownAtResponse>(ctx, managarm::posix::Errors::SUCCESS);
}

// UTIMENSAT handler
async::result<void> handleUtimensAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::UtimensAtRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests || logPaths, ctx, "UTIMENSAT");

	ViewPath relativeTo;
	smarter::shared_ptr<File, FileHandle> file;
	std::shared_ptr<FsNode> target = nullptr;

	if(!req->path().size() && (req->flags() & AT_EMPTY_PATH)) {
		target = ctx.self->fileContext()->getFile(req->fd())->associatedLink()->getTarget();
	} else {
		if(req->flags() & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH)) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return;
		}

		ResolveFlags resolveFlags = 0;
		if(req->flags() & AT_SYMLINK_NOFOLLOW) {
			resolveFlags |= resolveDontFollow;
		}

		if(req->fd() == AT_FDCWD) {
			relativeTo = ctx.self->fsContext()->getWorkingDirectory();
		} else {
			file = ctx.self->fileContext()->getFile(req->fd());
			if (!file) {
				co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
				co_return;
			}

			relativeTo = {file->associatedMount(), file->associatedLink()};
		}

		PathResolver resolver;
		resolver.setup(ctx.self->fsContext()->getRoot(),
				relativeTo, req->path(), ctx.self.get());
		auto resolveResult = co_await resolver.resolve(resolveFlags);
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

		target = resolver.currentLink()->getTarget();
	}

	std::optional<timespec> atime = std::nullopt;
	std::optional<timespec> mtime = std::nullopt;

	auto time = clk::getRealtime();
	if(req->atimeNsec() == UTIME_NOW) {
		atime = {time.tv_sec, time.tv_nsec};
	} else if(req->atimeNsec() != UTIME_OMIT) {
		if(req->atimeNsec() > 999'999'999) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return;
		}
		atime = {static_cast<time_t>(req->atimeSec()), static_cast<long>(req->atimeNsec())};
	}

	if(req->mtimeNsec() == UTIME_NOW) {
		mtime = {time.tv_sec, time.tv_nsec};
	} else if(req->mtimeNsec() != UTIME_OMIT) {
		if(req->mtimeNsec() > 999'999'999) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
			co_return;
		}
		mtime = {static_cast<time_t>(req->mtimeSec()), static_cast<long>(req->mtimeNsec())};
	}

	co_await target->utimensat(atime, mtime, time);

	co_await sendErrorResponse(ctx, managarm::posix::Errors::SUCCESS);
}

// OPENAT handler
async::result<void> handleOpenAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::recvBuffer(tail.data(), tail.size())
	);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::OpenAtRequest>(ctx.recv_head, tail);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
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
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	if (req->path().length() > PATH_MAX) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::NAME_TOO_LONG);
		co_return;
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
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->fd());

		if (!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	PathResolver resolver;
	resolver.setup(ctx.self->fsContext()->getRoot(),
			relative_to, req->path(), ctx.self.get());
	if(req->flags() & managarm::posix::OpenFlags::OF_CREATE) {
		auto resolveResult = co_await resolver.resolve(
				resolvePrefix | resolveNoTrailingSlash);
		if(!resolveResult) {
			if(resolveResult.error() == protocols::fs::Error::isDirectory) {
				// TODO: Verify additional constraints for sending EISDIR.
				co_await sendErrorResponse(ctx, managarm::posix::Errors::IS_DIRECTORY);
				co_return;
			} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
				co_await sendErrorResponse(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
				co_return;
			} else if(resolveResult.error() == protocols::fs::Error::notDirectory) {
				co_await sendErrorResponse(ctx, managarm::posix::Errors::NOT_A_DIRECTORY);
				co_return;
			} else if(resolveResult.error() == protocols::fs::Error::nameTooLong) {
				co_await sendErrorResponse(ctx, managarm::posix::Errors::NAME_TOO_LONG);
				co_return;
			} else {
				std::cout << "posix: Unexpected failure from resolve()" << std::endl;
				co_return;
			}
		}

		logRequest(logRequests || logPaths, ctx, "OPENAT", "create '{}'",
			ViewPath{resolver.currentView(), resolver.currentLink()}
			.getPath(ctx.self->fsContext()->getRoot()));

		if (!resolver.hasComponent()) {
			if ((req->flags() & managarm::posix::OpenFlags::OF_RDWR)
			|| (req->flags() & managarm::posix::OpenFlags::OF_WRONLY))
				co_await sendErrorResponse(ctx, managarm::posix::Errors::IS_DIRECTORY);
			else
				co_await sendErrorResponse(ctx, managarm::posix::Errors::ALREADY_EXISTS);
			co_return;
		}

		auto directory = resolver.currentLink()->getTarget();

		auto linkResult = co_await directory->getLinkOrCreate(ctx.self.get(), resolver.nextComponent(),
			req->mode() & ~ctx.self->fsContext()->getUmask(), req->flags() & managarm::posix::OpenFlags::OF_EXCLUSIVE);
		if (!linkResult) {
			co_await sendErrorResponse(ctx, linkResult.error() | toPosixProtoError);
			co_return;
		}
		auto link = linkResult.value();
		assert(link);
		auto node = link->getTarget();

		auto fileResult = co_await node->open(ctx.self.get(), resolver.currentView(), std::move(link),
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
				co_await sendErrorResponse(ctx, managarm::posix::Errors::IS_DIRECTORY);
				co_return;
			} else if(resolveResult.error() == protocols::fs::Error::fileNotFound) {
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

		logRequest(logRequests || logPaths, ctx, "OPENAT", "open '{}'",
			ViewPath{resolver.currentView(), resolver.currentLink()}
			.getPath(ctx.self->fsContext()->getRoot()));

		auto target = resolver.currentLink()->getTarget();
		if(req->flags() & managarm::posix::OpenFlags::OF_DIRECTORY) {
			if(target->getType() != VfsType::directory) {
				co_await sendErrorResponse(ctx, managarm::posix::Errors::NOT_A_DIRECTORY);
				co_return;
			}
		}

		if(req->flags() & managarm::posix::OpenFlags::OF_PATH) {
			auto dummyFile = smarter::make_shared<DummyFile>(resolver.currentView(), resolver.currentLink());
			DummyFile::serve(dummyFile);
			file = File::constructHandle(std::move(dummyFile));
		} else {
			// this can only be a symlink if O_NOFOLLOW has been passed
			if(target->getType() == VfsType::symlink) {
				co_await sendErrorResponse(ctx, managarm::posix::Errors::SYMBOLIC_LINK_LOOP);
				co_return;
			}

			auto fileResult = co_await target->open(ctx.self.get(), resolver.currentView(), resolver.currentLink(), semantic_flags);
			if(!fileResult) {
				if(fileResult.error() == Error::noBackingDevice) {
					co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_BACKING_DEVICE);
					co_return;
				} else if(fileResult.error() == Error::illegalArguments) {
					co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
					co_return;
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
		co_await sendErrorResponse(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
		co_return;
	}

	if(file->isTerminal() &&
		!(req->flags() & managarm::posix::OpenFlags::OF_NOCTTY) &&
        ctx.self->pgPointer() &&
		ctx.self->pgPointer()->getSession()->getSessionId() == (pid_t)ctx.self->pid() &&
		ctx.self->pgPointer()->getSession()->getControllingTerminal() == nullptr) {
		// POSIX 1003.1-2017 11.1.3
		auto cts = co_await file->getControllingTerminal();
		if(!cts) {
			std::cout << "posix: Unable to get controlling terminal (" << (int)cts.error() << ")" << std::endl;
		} else {
			cts.value()->assignSessionOf(ctx.self.get());
		}
	}

	if(req->flags() & managarm::posix::OpenFlags::OF_TRUNC) {
		auto result = co_await file->truncate(0);
		assert(result || result.error() == protocols::fs::Error::illegalOperationTarget);
	}
	auto fd = ctx.self->fileContext()->attachFile(file,
			req->flags() & managarm::posix::OpenFlags::OF_CLOEXEC);

	managarm::posix::SvrResponse resp;
	if (fd) {
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_fd(fd.value());
	} else {
		resp.set_error(fd.error() | toPosixProtoError);
	}

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

// MKNODAT handler
async::result<void> handleMknodAt(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::recvBuffer(tail.data(), tail.size())
		);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::MknodAtRequest>(ctx.recv_head, tail);

	if(!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests || logPaths, ctx, "MKNODAT", "path='{}' mode={:o} device={:#x}", req->path(), req->mode(), req->device());

	managarm::posix::SvrResponse resp;

	ViewPath relative_to;
	smarter::shared_ptr<File, FileHandle> file;

	if(!req->path().size()) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	if(req->dirfd() == AT_FDCWD) {
		relative_to = ctx.self->fsContext()->getWorkingDirectory();
	} else {
		file = ctx.self->fileContext()->getFile(req->dirfd());

		if(!file) {
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_SUCH_FD);
			co_return;
		}

		relative_to = {file->associatedMount(), file->associatedLink()};
	}

	// TODO: Add resolveNoTrailingSlash if not making a directory?
	PathResolver resolver;
	resolver.setup(ctx.self->fsContext()->getRoot(),
			relative_to, req->path(), ctx.self.get());
	auto resolveResult = co_await resolver.resolve(resolvePrefix);
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

	auto parent = resolver.currentLink()->getTarget();
	auto existsResult = co_await parent->getLink(resolver.nextComponent());
	if (existsResult) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ALREADY_EXISTS);
		co_return;
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
				co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				co_return;
			} else {
				std::cout << "posix: Unexpected failure from mkdev()" << std::endl;
				co_return;
			}
		}
	} else if(type == VfsType::fifo) {
		auto result = co_await parent->mkfifo(
			resolver.nextComponent(),
			req->mode() & ~ctx.self->fsContext()->getUmask()
		);
		if(!result) {
			if(result.error() == Error::illegalOperationTarget) {
				co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				co_return;
			} else {
				std::cout << "posix: Unexpected failure from mkfifo()" << std::endl;
				co_return;
			}
		}
	} else if(type == VfsType::socket) {
		auto result = co_await parent->mksocket(resolver.nextComponent());
		if(!result) {
			if(result.error() == Error::illegalOperationTarget) {
				co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
				co_return;
			} else {
				std::cout << "posix: Unexpected failure from mksocket()" << std::endl;
				co_return;
			}
		}
	} else {
		// TODO: Handle regular files.
		std::cout << "\e[31mposix: Creating regular files with mknod is not supported.\e[39m" << std::endl;
		co_await sendErrorResponse(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// UMASK handler
async::result<void> handleUmask(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::UmaskRequest>(ctx.recv_head);
	logRequest(logRequests, ctx, "UMASK", "newmask={:o}", req->newmask());

	managarm::posix::UmaskResponse resp;
	mode_t oldmask = ctx.self->fsContext()->setUmask(req->newmask());
	resp.set_oldmask(oldmask);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

} // namespace requests
