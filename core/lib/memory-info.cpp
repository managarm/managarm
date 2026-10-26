#include <async/mutex.hpp>
#include <bragi/helpers-std.hpp>
#include <core/memory-info.hpp>
#include <frg/mutex.hpp>
#include <kerncfg.bragi.hpp>
#include <optional>
#include <protocols/mbus/client.hpp>

namespace core {

namespace {

async::mutex kerncfgMutex;
std::optional<helix::UniqueLane> kerncfgLane;

async::result<helix::BorrowedLane> getKerncfgLane() {
	co_await kerncfgMutex.async_lock();
	frg::unique_lock lock{frg::adopt_lock, kerncfgMutex};

	if(!kerncfgLane) {
		auto filter = mbus_ng::Conjunction{{
			mbus_ng::EqualsFilter{"class", "kerncfg"}
		}};

		auto enumerator = mbus_ng::Instance::global().enumerate(filter);
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
		assert(events.size() == 1);

		auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
		kerncfgLane = (co_await entity.getRemoteLane()).unwrap();
	}

	co_return *kerncfgLane;
}

} // anonymous namespace

async::result<MemoryInformation> getMemoryInformation() {
	auto lane = co_await getKerncfgLane();

	managarm::kerncfg::GetMemoryInformationRequest req;
	auto [offer, sendReq, recvResp] = co_await helix_ng::exchangeMsgs(
		lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline()
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto resp = bragi::parse_head_only<managarm::kerncfg::GetMemoryInformationResponse>(recvResp);
	assert(resp);
	assert(resp->error() == managarm::kerncfg::Error::SUCCESS);

	co_return MemoryInformation{
		.totalMemory = resp->total_usable_memory() * resp->memory_unit(),
		.availableMemory = resp->available_memory() * resp->memory_unit(),
	};
}

} // namespace core
