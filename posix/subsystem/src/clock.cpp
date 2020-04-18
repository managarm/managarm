
#include <async/jump.hpp>
#include <helix/memory.hpp>
#include <protocols/clock/defs.hpp>
#include <protocols/mbus/client.hpp>

#include "clock.hpp"
#include "clock.pb.h"

namespace clk {

namespace {

async::jump foundTracker;

helix::UniqueLane trackerLane;
helix::UniqueDescriptor globalTrackerPageMemory;
helix::Mapping trackerPageMapping;

async::detached fetchTrackerPage() {
	managarm::clock::CntRequest req;
	req.set_req_type(managarm::clock::CntReqType::ACCESS_PAGE);

	auto ser = req.SerializeAsString();
	auto [offer, send_req, recv_resp, pull_memory] = co_await helix_ng::exchangeMsgs(
		trackerLane,
		helix_ng::offer(
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_memory.error());

	managarm::clock::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::clock::Error::SUCCESS);
	globalTrackerPageMemory = pull_memory.descriptor();
	
	trackerPageMapping = helix::Mapping{globalTrackerPageMemory, 0, 0x1000};
	
	foundTracker.trigger();
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
	co_await foundTracker.async_wait();
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

} // namespace clk
