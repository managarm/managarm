
#include <string.h>
#include <iostream>

#include <cofiber.hpp>
#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../device.hpp"
#include "../util.hpp"
#include "../vfs.hpp"

namespace block_subsystem {

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
		mbus::EqualsFilter("unix.devtype", "block")
	});
	
	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		std::cout << "POSIX: Installing block device "
				<< std::get<mbus::StringItem>(properties.at("unix.devname")).value << std::endl;

		auto lane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
		auto device = std::make_shared<Device>(VfsType::blockDevice,
				std::get<mbus::StringItem>(properties.at("unix.devname")).value,
				std::move(lane));
		// We use 8 here, the major for SCSI devices and allocate minors sequentially.
		// Note that this is not really correct as the minor of a partition
		// depends on the minor of the whole device (see Linux devices.txt documentation).
		device->assignId({8, minorAllocator.allocate()});
		blockRegistry.install(device);
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

} // namespace block_subsystem

