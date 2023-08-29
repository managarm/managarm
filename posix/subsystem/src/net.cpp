#include "net.hpp"

#include <async/oneshot-event.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/fs/client.hpp>

namespace net {
namespace {
helix::UniqueLane netserverLane;
async::oneshot_event foundNetserver;
} // namespace

async::result<void> enumerateNetserver() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("class", "netserver")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) -> async::detached {
		std::cout << "POSIX: found netserver" << std::endl;

		netserverLane = helix::UniqueLane(co_await entity.bind());
		foundNetserver.raise();
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
	co_await foundNetserver.wait();

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
