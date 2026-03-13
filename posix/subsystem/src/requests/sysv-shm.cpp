#include <sys/ipc.h>
#include <sys/shm.h>

#include "common.hpp"
#include "../sysv-shm.hpp"
namespace requests {

async::result<void> handleShmGet(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::ShmGetRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SHM_GET", "key={}, size={}, flags={:#x}",
		req->key(), req->size(), req->flags());

	key_t key = req->key();
	size_t size = req->size();
	int flags = req->flags();

	std::expected<std::shared_ptr<shm::ShmSegment>, Error> result;
	if (key == IPC_PRIVATE) {
		result = shm::createPrivateSegment(size, flags & 0777,
				ctx.self->pid(), ctx.self->threadGroup()->uid(), ctx.self->threadGroup()->gid());
	} else {
		result = shm::getOrCreateSegment(key, size, flags,
				ctx.self->pid(), ctx.self->threadGroup()->uid(), ctx.self->threadGroup()->gid());
	}

	if (!result) {
		co_await sendErrorResponse<managarm::posix::ShmGetResponse>(ctx, result.error() | toPosixProtoError);
		co_return;
	}
	auto segment = result.value();

	managarm::posix::ShmGetResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_shmid(segment->shmid);

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

async::result<void> handleShmAt(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::ShmAtRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SHM_AT", "shmid={}, shmaddr={:#x}, flags={:#x}, pid={}",
		req->shmid(), req->shmaddr(), req->flags(), ctx.self->pid());

	if (req->flags() & ~(SHM_RDONLY | SHM_EXEC)) {
		std::println("posix: Unsupported SHM_AT flags: {}", req->flags());
		co_await sendErrorResponse<managarm::posix::ShmAtResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	auto segment = shm::findById(req->shmid());
	if (!segment) {
		co_await sendErrorResponse<managarm::posix::ShmAtResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	uint32_t nativeFlags = kHelMapProtRead;
	if (!(req->flags() & SHM_RDONLY))
		nativeFlags |= kHelMapProtWrite;
	if (req->flags() & SHM_EXEC)
		nativeFlags |= kHelMapProtExecute;
	if (req->shmaddr())
		nativeFlags |= kHelMapFixed;

	// Create area and map it
	auto area = Area::makeShm(segment);
	auto result = ctx.self->vmContext()->mapArea(req->shmaddr(), nativeFlags, std::move(area));
	if (!result) {
		co_await sendErrorResponse<managarm::posix::ShmAtResponse>(ctx, result.error() | toPosixProtoError);
		co_return;
	}

	// Update segment metadata.
	segment->nattch++;
	segment->lpid = ctx.self->pid();
	segment->atime = 0;

	managarm::posix::ShmAtResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);
	resp.set_address(reinterpret_cast<uintptr_t>(result.value()));

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

async::result<void> handleShmDt(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::ShmDtRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SHM_DT", "address={:#x}", req->address());

	auto result = ctx.self->vmContext()->unmapShm(reinterpret_cast<void*>(req->address()), ctx.self->pid());
	if (!result) {
		co_await sendErrorResponse<managarm::posix::ShmDtResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	managarm::posix::ShmDtResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

async::result<void> handleShmCtl(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::ShmCtlRequest>(ctx.recv_head);
	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SHM_CTL", "shmid={}, cmd={}", req->shmid(), req->cmd());

	auto segment = shm::findById(req->shmid());
	if (!segment) {
		co_await sendErrorResponse<managarm::posix::ShmCtlResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	managarm::posix::ShmCtlResponse resp;

	// TODO: Both IPC_STAT and IPC_RMID need to check permissions.
	switch (req->cmd()) {
	case IPC_STAT:
		resp.set_error(managarm::posix::Errors::SUCCESS);
		resp.set_perm_key(segment->key);
		resp.set_perm_uid(segment->uid);
		resp.set_perm_gid(segment->gid);
		resp.set_perm_cuid(segment->cuid);
		resp.set_perm_cgid(segment->cgid);
		resp.set_perm_mode(segment->mode);
		resp.set_perm_seq(segment->seq);
		resp.set_shm_segsz(segment->size);
		resp.set_shm_atime(segment->atime);
		resp.set_shm_dtime(segment->dtime);
		resp.set_shm_ctime(segment->ctime);
		resp.set_shm_cpid(segment->cpid);
		resp.set_shm_lpid(segment->lpid);
		resp.set_shm_nattch(segment->nattch);
		break;

	case IPC_RMID:
		segment->markedForRemoval = true;
		if (!segment->nattch)
			shm::removeSegment(segment);
		resp.set_error(managarm::posix::Errors::SUCCESS);
		break;

	default:
		std::println("posix: Unsupported SHM_CTL command {}", req->cmd());
		co_await sendErrorResponse<managarm::posix::ShmCtlResponse>(ctx, managarm::posix::Errors::ILLEGAL_ARGUMENTS);
		co_return;
	}

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

} // namespace requests
