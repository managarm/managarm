#include "common.hpp"
#include "../requests.hpp"
#include "../procfs.hpp"
#include "../device.hpp"
#include "../pts.hpp"
#include "../sysfs.hpp"
#include "../tmp_fs.hpp"
#include "../cgroupfs.hpp"
#include <unistd.h>
#include <kerncfg.bragi.hpp>
#include <hw.bragi.hpp>

namespace requests {

// REBOOT handler
async::result<void> handleReboot(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::RebootRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "REBOOT", "command={}", req->cmd());

	if(ctx.self->threadGroup()->uid() != 0) {
		co_await sendErrorResponse(ctx, managarm::posix::Errors::INSUFFICIENT_PERMISSION);
		co_return;
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

	auto [send_resp] = co_await helix_ng::exchangeMsgs(ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// MOUNT handler
async::result<void> handleMount(RequestContext& ctx) {
	std::vector<uint8_t> tail(ctx.preamble.tail_size());
	auto [recv_tail] = co_await helix_ng::exchangeMsgs(
			ctx.conversation,
			helix_ng::recvBuffer(tail.data(), tail.size())
		);
	HEL_CHECK(recv_tail.error());

	logBragiRequest(ctx, tail);
	auto req = bragi::parse_head_tail<managarm::posix::MountRequest>(ctx.recv_head, tail);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "MOUNT", "fstype={} on={} to={}", req->fs_type(), req->path(), req->target_path());

	auto resolveResult = co_await resolve(ctx.self->fsContext()->getRoot(),
			ctx.self->fsContext()->getWorkingDirectory(), req->target_path(), ctx.self.get());
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
		if(req->fs_type() != "ext2" && req->fs_type() != "btrfs") {
			std::cout << "posix: Trying to mount unsupported FS of type: " << req->fs_type() << std::endl;
			co_await sendErrorResponse(ctx, managarm::posix::Errors::NO_BACKING_DEVICE);
			co_return;
		}

		auto sourceResult = co_await resolve(ctx.self->fsContext()->getRoot(),
				ctx.self->fsContext()->getWorkingDirectory(), req->path(), ctx.self.get());
		if(!sourceResult) {
			if(sourceResult.error() == protocols::fs::Error::fileNotFound) {
				co_await sendErrorResponse(ctx, managarm::posix::Errors::FILE_NOT_FOUND);
				co_return;
			} else if(sourceResult.error() == protocols::fs::Error::notDirectory) {
				co_await sendErrorResponse(ctx, managarm::posix::Errors::NOT_A_DIRECTORY);
				co_return;
			} else {
				std::cout << "posix: Unexpected failure from resolve()" << std::endl;
				co_return;
			}
		}
		auto source = sourceResult.value();
		assert(source.second);
		assert(source.second->getTarget()->getType() == VfsType::blockDevice);
		auto device = blockRegistry.get(source.second->getTarget()->readDevice());
		auto link = co_await device->mount(req->fs_type());
		co_await target.first->mount(target.second, std::move(link), source);
	}

	logRequest(logRequests, ctx, "MOUNT", "succeeded");

	managarm::posix::SvrResponse resp;
	resp.set_error(managarm::posix::Errors::SUCCESS);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
				ctx.conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// SYSCONF handler
async::result<void> handleSysconf(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::SysconfRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "SYSCONF");

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
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// GET_MEMORY_INFORMATION handler
async::result<void> handleGetMemoryInformation(RequestContext& ctx) {
	auto req = bragi::parse_head_only<managarm::posix::GetMemoryInformationRequest>(ctx.recv_head);

	if (!req) {
		std::cout << "posix: Rejecting request due to decoding failure" << std::endl;
		co_return;
	}

	logRequest(logRequests, ctx, "GET_MEMORY_INFORMATION");

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
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);
	HEL_CHECK(sendResp.error());
	logBragiReply(ctx, resp);
}

} // namespace requests
