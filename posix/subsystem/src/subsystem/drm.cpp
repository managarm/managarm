
#include <string.h>
#include <iostream>

#include <cofiber.hpp>
#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../device.hpp"
#include "../vfs.hpp"

namespace drm_system {

namespace {

struct Device : UnixDevice {
	Device(VfsType type, std::string name, helix::UniqueLane lane)
	: UnixDevice{type},
			_name{std::move(name)}, _lane{std::move(lane)} { }

	std::string getName() override {
		return _name;
	}
	
	FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<FsLink> link) override {
		return openExternalDevice(_lane, std::move(link));
	}

	FutureMaybe<std::shared_ptr<FsLink>> mount() {
		return mountExternalDevice(_lane);
	}


private:
	std::string _name;
	helix::UniqueLane _lane;
};

} // anonymous namepsace

COFIBER_ROUTINE(cofiber::no_future, run(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("unix.subsystem", "drm")
	});
	
	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		std::cout << "POSIX: Installing DRM device "
				<< std::get<mbus::StringItem>(properties.at("unix.devname")).value << std::endl;

		auto lane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
		auto device = std::make_shared<Device>(VfsType::charDevice,
				std::get<mbus::StringItem>(properties.at("unix.devname")).value,
				std::move(lane));
		charRegistry.install(device);
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

} // namespace drm_system

