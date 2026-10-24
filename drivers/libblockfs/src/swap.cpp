#include "swap.hpp"

#include <stdio.h>

#include "bragi/helpers-std.hpp"
#include "swap.bragi.hpp"

namespace blockfs {

namespace {

constexpr size_t pageSize = 0x1000;

bool isEndOfLane(HelError error) {
	return error == kHelErrEndOfLane || error == kHelErrLaneShutdown;
}

} // namespace

async::detached serveSwapTarget(helix::UniqueLane lane, gpt::Partition *partition) {
	auto numPages = co_await partition->getSize() / pageSize;

	// Manage is required to map the backing memory without dirty tracking.
	auto [pullDesc] = co_await helix_ng::exchangeMsgs(
	    lane,
	    helix_ng::pullDescriptor(kHelRightRead | kHelRightWrite | kHelRightAssign | kHelRightManage)
	);
	if (isEndOfLane(pullDesc.error()))
		co_return;
	HEL_CHECK(pullDesc.error());
	auto backingHandle = pullDesc.descriptor();

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
			printf(
			    "blockfs: Swap request at page %lu with %lu pages exceeds the partition\n",
			    req.offset(),
			    req.count()
			);
			error = managarm::swap::Errors::OTHER;
		} else {
			// The swap server can only complete the request once the range is no longer mapped,
			// hence the view must be gone before we respond.
			auto sector = (req.offset() * pageSize) >> partition->sectorShift;
			auto view = partition->pagePool->realm()->importMemory(
			    backingHandle,
			    req.offset() * pageSize,
			    req.count() * pageSize,
			    kHelMapNoDirtyTracking
			);

			try {
				switch (req.type()) {
					case managarm::swap::ManageType::INITIALIZE:
						co_await partition->readSectors(sector, view);
						break;
					case managarm::swap::ManageType::WRITEBACK:
						co_await partition->writeSectors(sector, view);
						break;
				}
			} catch (const std::exception &e) {
				printf("blockfs: Swap I/O at page %lu failed: %s\n", req.offset(), e.what());
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

} // namespace blockfs
