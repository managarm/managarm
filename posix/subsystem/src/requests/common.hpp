#pragma once

#include <memory>
#include <functional>
#include <span>
#include <format>
#include <vector>

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>
#include <bragi/helpers-std.hpp>
#include <posix.bragi.hpp>
#include <core/clock.hpp>

#include "../debug-options.hpp"
#include "../process.hpp"
#include "../ostrace.hpp"
#include "../clocks.hpp"
#include "../pidfd.hpp"

namespace requests {

// Context structure that holds all the state needed by request handlers
struct RequestContext {
	std::shared_ptr<Process> self;
	std::shared_ptr<Generation> generation;
	helix::UniqueDescriptor& conversation;
	bragi::preamble& preamble;
	helix_ng::RecvInlineResult& recv_head;
	
	// Timing information for ostrace
	timespec& requestTimestamp;
	protocols::ostrace::Timer& timer;
};

// Type alias for request handler functions
using RequestHandler = async::result<void>(*)(RequestContext& ctx);

// Helper functions available to all handlers
constexpr void logRequest(bool condition, RequestContext& ctx, std::string_view name) {
	if(condition) {
		std::cout << "posix: [" << ctx.self->pid() << "] " << name << std::endl;
	}
}

template<class... Args>
constexpr void logRequest(bool condition, RequestContext& ctx, std::string_view name,
                std::format_string<Args...> fmt, Args&&... args) {
	if(condition) {
		std::cout << "posix: [" << ctx.self->pid() << "] " << name << " "
		          << std::format(fmt, std::forward<Args>(args)...) << std::endl;
	}
}

constexpr void logBragiRequest(RequestContext& ctx, std::span<uint8_t> tail) {
	if(!posix::ostContext.isActive())
		return;

	ctx.requestTimestamp = clk::getTimeSinceBoot();
	posix::ostContext.emitWithTimestamp(
		posix::ostEvtRequest,
		(ctx.requestTimestamp.tv_sec * 1'000'000'000) + ctx.requestTimestamp.tv_nsec,
		posix::ostAttrPid(ctx.self->tid()),
		posix::ostAttrTime((ctx.requestTimestamp.tv_sec * 1'000'000'000) + ctx.requestTimestamp.tv_nsec),
		posix::ostBragi(std::span<uint8_t>{reinterpret_cast<uint8_t *>(ctx.recv_head.data()), 
		                                    static_cast<size_t>(ctx.recv_head.size())}, tail)
	);
}

template<typename Response>
constexpr void logBragiReply(RequestContext& ctx, Response& resp) {
	if(!posix::ostContext.isActive())
		return;

	auto ts = clk::getTimeSinceBoot();
	std::string replyHead;
	std::string replyTail;
	replyHead.resize(resp.size_of_head());
	replyTail.resize(resp.size_of_tail());
	bragi::limited_writer headWriter{replyHead.data(), replyHead.size()};
	bragi::limited_writer tailWriter{replyTail.data(), replyTail.size()};
	auto headOk = resp.encode_head(headWriter);
	auto tailOk = resp.encode_tail(tailWriter);
	assert(headOk);
	assert(tailOk);
	posix::ostContext.emitWithTimestamp(
		posix::ostEvtRequest,
		(ts.tv_sec * 1'000'000'000) + ts.tv_nsec,
		posix::ostAttrRequest(ctx.preamble.id()),
		posix::ostAttrTime((ctx.requestTimestamp.tv_sec * 1'000'000'000) + ctx.requestTimestamp.tv_nsec),
		posix::ostAttrPid(ctx.self->tid()),
		posix::ostBragi({reinterpret_cast<uint8_t *>(replyHead.data()), replyHead.size()}, 
		                {reinterpret_cast<uint8_t *>(replyTail.data()), replyTail.size()})
	);
}

template<typename Message = managarm::posix::SvrResponse>
inline async::result<void> sendErrorResponse(RequestContext& ctx, managarm::posix::Errors err) {
	Message resp;
	resp.set_error(err);

	auto [send_resp] = co_await helix_ng::exchangeMsgs(
		ctx.conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(send_resp.error());
	logBragiReply(ctx, resp);
}

// From fd.cpp
async::result<void> handleDup2(RequestContext& ctx);
async::result<void> handleIsTty(RequestContext& ctx);
async::result<void> handleIoctlFioclex(RequestContext& ctx);
async::result<void> handleClose(RequestContext& ctx);

// From filesystem.cpp
async::result<void> handleChroot(RequestContext& ctx);
async::result<void> handleChdir(RequestContext& ctx);
async::result<void> handleAccessAt(RequestContext& ctx);
async::result<void> handleMkdirAt(RequestContext& ctx);
async::result<void> handleMkfifoAt(RequestContext& ctx);
async::result<void> handleLinkAt(RequestContext& ctx);
async::result<void> handleSymlinkAt(RequestContext& ctx);
async::result<void> handleReadlinkAt(RequestContext& ctx);
async::result<void> handleRenameAt(RequestContext& ctx);
async::result<void> handleUnlinkAt(RequestContext& ctx);
async::result<void> handleRmdir(RequestContext& ctx);
async::result<void> handleFstatAt(RequestContext& ctx);
async::result<void> handleFstatfs(RequestContext& ctx);
async::result<void> handleFchmodAt(RequestContext& ctx);
async::result<void> handleFchownAt(RequestContext& ctx);
async::result<void> handleUtimensAt(RequestContext& ctx);
async::result<void> handleOpenAt(RequestContext& ctx);
async::result<void> handleMknodAt(RequestContext& ctx);
async::result<void> handleUmask(RequestContext& ctx);

// From special-files.cpp
async::result<void> handleInotifyCreate(RequestContext& ctx);
async::result<void> handleInotifyAdd(RequestContext& ctx);
async::result<void> handleInotifyRm(RequestContext& ctx);
async::result<void> handleEventfdCreate(RequestContext& ctx);
async::result<void> handleTimerFdCreate(RequestContext& ctx);
async::result<void> handleTimerFdSet(RequestContext& ctx);
async::result<void> handleTimerFdGet(RequestContext& ctx);
async::result<void> handlePidfdOpen(RequestContext& ctx);
async::result<void> handlePidfdSendSignal(RequestContext& ctx);
async::result<void> handlePidfdGetPid(RequestContext& ctx);

// From memory.cpp
async::result<void> handleVmMap(RequestContext& ctx);
async::result<void> handleMemFdCreate(RequestContext& ctx);

// From uid-gid.cpp
async::result<void> handleGetPid(RequestContext& ctx);
async::result<void> handleGetPpid(RequestContext& ctx);
async::result<void> handleGetUid(RequestContext& ctx);
async::result<void> handleSetUid(RequestContext& ctx);
async::result<void> handleGetEuid(RequestContext& ctx);
async::result<void> handleSetEuid(RequestContext& ctx);
async::result<void> handleGetGid(RequestContext& ctx);
async::result<void> handleGetEgid(RequestContext& ctx);
async::result<void> handleSetGid(RequestContext& ctx);
async::result<void> handleSetEgid(RequestContext& ctx);
async::result<void> handleGetGroups(RequestContext& ctx);
async::result<void> handleSetGroups(RequestContext& ctx);

// From process.cpp
async::result<void> handleWaitId(RequestContext& ctx);
async::result<void> handleSetAffinity(RequestContext& ctx);
async::result<void> handleGetAffinity(RequestContext& ctx);
async::result<void> handleGetPgid(RequestContext& ctx);
async::result<void> handleSetPgid(RequestContext& ctx);
async::result<void> handleGetSid(RequestContext& ctx);
async::result<void> handleParentDeathSignal(RequestContext& ctx);
async::result<void> handleProcessDumpable(RequestContext& ctx);
async::result<void> handleSetResourceLimit(RequestContext& ctx);

// From socket.cpp
async::result<void> handleNetserver(RequestContext& ctx);
async::result<void> handleSocket(RequestContext& ctx);
async::result<void> handleSockpair(RequestContext& ctx);
async::result<void> handleAccept(RequestContext& ctx);

// From system.cpp
async::result<void> handleReboot(RequestContext& ctx);
async::result<void> handleMount(RequestContext& ctx);
async::result<void> handleSysconf(RequestContext& ctx);
async::result<void> handleGetMemoryInformation(RequestContext& ctx);

// From timer.cpp
async::result<void> handleSetIntervalTimer(RequestContext& ctx);
async::result<void> handleTimerCreate(RequestContext& ctx);
async::result<void> handleTimerSet(RequestContext& ctx);
async::result<void> handleTimerGet(RequestContext& ctx);
async::result<void> handleTimerDelete(RequestContext& ctx);

} // namespace requests
