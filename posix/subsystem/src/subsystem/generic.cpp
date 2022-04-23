#include <string.h>
#include <iostream>

#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../device.hpp"
#include "../util.hpp"
#include "../vfs.hpp"

namespace generic_subsystem {

namespace {

id_allocator<uint32_t> minorAllocator;

struct Subsystem {
	Subsystem() {
		minorAllocator.use_range(0);
	}
} subsystem;

struct Device final : UnixDevice {
	Device(VfsType type, std::string name, helix::UniqueLane lane)
	: UnixDevice{type},
			_name{std::move(name)}, _lane{std::move(lane)} { }

	std::string nodePath() override {
		return _name;
	}
	
	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(_lane, std::move(mount), std::move(link), semantic_flags);
	}

	FutureMaybe<std::shared_ptr<FsLink>> mount() override {
		return mountExternalDevice(_lane);
	}

private:
	std::string _name;
	helix::UniqueLane _lane;
};

} // anonymous namepsace

async::detached run() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto blockFilter = mbus::Conjunction({
		mbus::EqualsFilter("generic.devtype", "block")
	});

	auto charFilter = mbus::Conjunction({
		mbus::EqualsFilter("generic.devtype", "char")
	});
	
	auto blockHandler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
		std::cout << "POSIX: Installing block device "
				<< std::get<mbus::StringItem>(properties.at("generic.devname")).value << std::endl;

		auto lane = helix::UniqueLane(co_await entity.bind());
		auto device = std::make_shared<Device>(VfsType::blockDevice,
				std::get<mbus::StringItem>(properties.at("generic.devname")).value,
				std::move(lane));
		// We use 240 here, the major for block devices local and experimental use and allocate minors sequentially.
		device->assignId({240, minorAllocator.allocate()});
		blockRegistry.install(device);
	});

	auto charHandler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
		std::cout << "POSIX: Installing char device "
				<< std::get<mbus::StringItem>(properties.at("generic.devname")).value << std::endl;

		auto lane = helix::UniqueLane(co_await entity.bind());
		auto device = std::make_shared<Device>(VfsType::charDevice,
				std::get<mbus::StringItem>(properties.at("generic.devname")).value,
				std::move(lane));
		// We use 234 here, the major for char devices dynamic allocation and allocate minors sequentially.
		device->assignId({234, minorAllocator.allocate()});
		charRegistry.install(device);
	});

	co_await root.linkObserver(std::move(blockFilter), std::move(blockHandler));
	co_await root.linkObserver(std::move(charFilter), std::move(charHandler));
}

} // namespace generic_subsystem
