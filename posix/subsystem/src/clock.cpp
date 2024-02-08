
#include <async/oneshot-event.hpp>
#include <helix/memory.hpp>
#include <protocols/clock/defs.hpp>
#include <protocols/mbus/client.hpp>
#include <bragi/helpers-std.hpp>

#include "clock.hpp"
#include <clock.bragi.hpp>

namespace clk {

namespace {

helix::UniqueLane trackerLane;
helix::UniqueDescriptor globalTrackerPageMemory;
helix::Mapping trackerPageMapping;

async::result<void> fetchTrackerPage() {
	managarm::clock::AccessPageRequest req;

	auto [offer, sendReq, recvResp, pullMemory] = co_await helix_ng::exchangeMsgs(
		trackerLane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(pullMemory.error());

	auto resp = *bragi::parse_head_only<managarm::clock::SvrResponse>(recvResp);

	recvResp.reset();
	assert(resp.error() == managarm::clock::Error::SUCCESS);

	globalTrackerPageMemory = pullMemory.descriptor();

	trackerPageMapping = helix::Mapping{globalTrackerPageMemory, 0, 0x1000};
}

} // anonymous namespace

helix::BorrowedDescriptor trackerPageMemory() {
	return globalTrackerPageMemory;
}

async::result<void> enumerateTracker() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "clocktracker"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	trackerLane = (co_await entity.getRemoteLane()).unwrap();
	co_await fetchTrackerPage();
}

struct timespec getRealtime() {
	auto page = reinterpret_cast<TrackerPage *>(trackerPageMapping.get());

	// Start the seqlock read.
	auto seqlock = __atomic_load_n(&page->seqlock, __ATOMIC_ACQUIRE);
	assert(!(seqlock & 1));

	// Perform the actual loads.
	auto ref = __atomic_load_n(&page->refClock, __ATOMIC_RELAXED);
	auto base = __atomic_load_n(&page->baseRealtime, __ATOMIC_RELAXED);

	// Finish the seqlock read.
	__atomic_thread_fence(__ATOMIC_ACQUIRE);
	assert(__atomic_load_n(&page->seqlock, __ATOMIC_RELAXED) == seqlock);

	// Calculate the current time.
	uint64_t now;
	HEL_CHECK(helGetClock(&now));

	int64_t realtime = base + (now - ref);

	struct timespec result;
	result.tv_sec = realtime / 1'000'000'000;
	result.tv_nsec = realtime % 1'000'000'000;
	return result;
}

struct timespec getTimeSinceBoot() {
	uint64_t now;
	HEL_CHECK(helGetClock(&now));

	struct timespec result;
	result.tv_sec = now / 1'000'000'000;
	result.tv_nsec = now % 1'000'000'000;
	return result;
}

} // namespace clk
