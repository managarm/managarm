#include "net.hpp"

#include <async/oneshot-event.hpp>
#include <protocols/mbus/client.hpp>

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
}

async::result<helix::BorrowedLane> getNetLane() {
	co_await foundNetserver.wait();
	co_return netserverLane;
}

} // namespace net
