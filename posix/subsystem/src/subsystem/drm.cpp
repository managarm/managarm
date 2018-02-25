
#include <string.h>
#include <iostream>

#include <cofiber.hpp>
#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../device.hpp"
#include "../util.hpp"
#include "../vfs.hpp"

namespace drm_subsystem {

namespace {

id_allocator<uint32_t> minorAllocator;

struct Subsystem {
	Subsystem() {
		minorAllocator.use_range(0);
	}
} subsystem;

struct Device : UnixDevice {
	Device(VfsType type, std::string name, helix::UniqueLane lane)
	: UnixDevice{type},
			_name{std::move(name)}, _lane{std::move(lane)} { }

	std::string getName() override {
		return _name;
	}
	
	FutureMaybe<std::shared_ptr<File>> open(std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(_lane, std::move(link), semantic_flags);
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
		// The minor is only correct for card* devices but not for control* and render*.
		device->assignId({226, minorAllocator.allocate()});
		charRegistry.install(device);
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

} // namespace drm_subsystem

