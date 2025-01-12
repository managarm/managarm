#include "subsystem.hpp"

namespace nvme {

async::result<void> Subsystem::run() {
	mbus_ng::Properties descriptor{
		{"class", mbus_ng::StringItem{"nvme-subsystem"}},
	};

	auto subsystem_entity = (co_await mbus_ng::Instance::global().createEntity(
		"nvme-subsystem", descriptor)).unwrap();

	mbusId_ = subsystem_entity.id();

	[] (mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			// TODO: do we need to serve something on localLane?
		}
	}(std::move(subsystem_entity));
}

} // namespace nvme
