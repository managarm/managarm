#include "swap.hpp"

#include <print>
#include <string>

#include <protocols/mbus/client.hpp>

#include "bragi/helpers-std.hpp"
#include "swap.bragi.hpp"

namespace blockfs {

namespace {

constexpr size_t pageSize = 0x1000;

bool isEndOfLane(HelError error) {
	return error == kHelErrEndOfLane || error == kHelErrLaneShutdown;
}

async::detached serveSwapTarget(helix::UniqueLane lane, std::shared_ptr<SwapTarget> target) {
	// Manage is required to map the backing memory without dirty tracking.
	auto [pullDesc] = co_await helix_ng::exchangeMsgs(
	    lane,
	    helix_ng::pullDescriptor(kHelRightRead | kHelRightWrite | kHelRightAssign | kHelRightManage)
	);
	if (isEndOfLane(pullDesc.error()))
		co_return;
	HEL_CHECK(pullDesc.error());
	auto backingHandle = pullDesc.descriptor();

	auto numPages = co_await target->getSwapSize() / pageSize;
	auto pool = target->getSwapPool();
	while (true) {
		auto [accept, recvHead] =
		    co_await helix_ng::exchangeMsgs(lane, helix_ng::accept(helix_ng::recvInline()));
		if (isEndOfLane(accept.error()))
			co_return;
		HEL_CHECK(accept.error());
		HEL_CHECK(recvHead.error());

		auto preamble = bragi::read_preamble(recvHead);
		if (preamble.id() != managarm::swap::ManageRequest::message_id)
			continue;

		auto req = *bragi::parse_head_only<managarm::swap::ManageRequest>(recvHead);

		auto error = managarm::swap::Errors::SUCCESS;
		if (req.count() > numPages || req.offset() > numPages - req.count()) {
			std::println(
			    "blockfs: Swap request at page {} with {} pages exceeds the partition",
			    req.offset(),
			    req.count()
			);
			error = managarm::swap::Errors::OTHER;
		} else {
			// The swap server can only complete the request once the range is no longer mapped,
			// hence the view must be gone before we respond.
			auto view = pool->realm()->importMemory(
			    backingHandle,
			    req.offset() * pageSize,
			    req.count() * pageSize,
			    kHelMapNoDirtyTracking
			);

			std::expected<void, Error> ioRes;
			switch (req.type()) {
				case managarm::swap::ManageType::INITIALIZE:
					ioRes = co_await target->readSwap(req.offset() * pageSize, view);
					break;
				case managarm::swap::ManageType::WRITEBACK:
					ioRes = co_await target->writeSwap(req.offset() * pageSize, view);
					break;
			}

			if (!ioRes) {
				std::println("blockfs: Swap I/O at page {} failed", req.offset());
				error = managarm::swap::Errors::OTHER;
			}
		}

		managarm::swap::ManageResponse resp;
		resp.set_error(error);

		auto [sendHead] = co_await helix_ng::exchangeMsgs(
		    accept.descriptor(), helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
		);
		if (isEndOfLane(sendHead.error()))
			co_return;
		HEL_CHECK(sendHead.error());
	}
}

} // namespace

async::result<protocols::fs::Error> swapon(std::shared_ptr<SwapTarget> target) {
	auto enableRes = co_await target->enableSwap();
	if (!enableRes)
		co_return enableRes.error() | protocols::fs::toFsProtoError;

	mbus_ng::Properties descriptor{
	    {"class", mbus_ng::StringItem{"swap-target"}},
	    {"size", mbus_ng::StringItem{std::to_string(co_await target->getSwapSize())}},
	};
	auto entity =
	    (co_await mbus_ng::Instance::global().createEntity("swap-target", descriptor)).unwrap();

	[](mbus_ng::EntityManager entity, std::shared_ptr<SwapTarget> target) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			serveSwapTarget(std::move(localLane), target);
		}
	}(std::move(entity), std::move(target));

	co_return protocols::fs::Error::none;
}

} // namespace blockfs
