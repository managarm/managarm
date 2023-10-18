
#include <async/oneshot-event.hpp>
#include <helix/memory.hpp>
#include <protocols/clock/defs.hpp>
#include <protocols/mbus/client.hpp>
#include <bragi/helpers-std.hpp>

#include "clock.hpp"
#include <clock.bragi.hpp>

namespace clk {

namespace {

async::oneshot_event foundTracker;

helix::UniqueLane trackerLane;
helix::UniqueDescriptor globalTrackerPageMemory;
helix::Mapping trackerPageMapping;

async::detached fetchTrackerPage() {
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

	foundTracker.raise();
}

} // anonymous namespace

helix::BorrowedDescriptor trackerPageMemory() {
	return globalTrackerPageMemory;
}

async::result<void> enumerateTracker() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "clocktracker")
	});
	
	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
		std::cout << "POSIX: Found clocktracker" << std::endl;

		trackerLane = helix::UniqueLane(co_await entity.bind());
		fetchTrackerPage();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundTracker.wait();
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
