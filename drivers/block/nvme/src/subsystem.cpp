#include "subsystem.hpp"

namespace nvme {

async::result<void> Subsystem::run() {
	mbus_ng::Properties descriptor{
		{"class", mbus_ng::StringItem{"nvme-subsystem"}},
	};

	mbusEntity_ = std::make_unique<mbus_ng::EntityManager>((co_await mbus_ng::Instance::global().createEntity(
		"nvme-subsystem", descriptor)).unwrap());
}

} // namespace nvme
