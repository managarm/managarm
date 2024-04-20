#include "net.hpp"

#include <async/oneshot-event.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/fs/client.hpp>

namespace net {
namespace {
helix::UniqueLane netserverLane;
async::oneshot_event foundNetserver;
} // namespace anonymous

async::result<void> enumerateNetserver() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "netserver"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	std::cout << "POSIX: found netserver" << std::endl;
	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	netserverLane = (co_await entity.getRemoteLane()).unwrap();
	foundNetserver.raise();

	managarm::fs::InitializePosixLane req;

	auto req_data = req.SerializeAsString();

	auto [offer, send_req] = co_await helix_ng::exchangeMsgs(
		netserverLane,
		helix_ng::offer(
			helix_ng::sendBuffer(req_data.data(), req_data.size())
		)
	);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
}

async::result<helix::BorrowedLane> getNetLane() {
	co_await foundNetserver.wait();
	co_return netserverLane;
}

} // namespace net
