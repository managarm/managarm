
#include <string.h>
#include <iostream>

#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../device.hpp"
#include "../util.hpp"
#include "../vfs.hpp"
#include "../drvcore.hpp"
#include "pci.hpp"

namespace block_subsystem {

namespace {

drvcore::ClassSubsystem *sysfsSubsystem;

id_allocator<uint32_t> minorAllocator;
id_allocator<uint32_t> diskAllocator;
std::unordered_map<int64_t, char> diskNames;

struct Subsystem {
	Subsystem() {
		minorAllocator.use_range(0);
		diskAllocator.use_range(0);
	}
} subsystem;

struct Device final : UnixDevice, drvcore::BlockDevice {
	Device(VfsType type, std::string name, helix::UniqueLane lane,
			std::shared_ptr<drvcore::Device> parent)
	: UnixDevice{type},
			drvcore::BlockDevice{sysfsSubsystem, std::move(parent), name,
					this},
			_name{std::move(name)}, _lane{std::move(lane)} { }

	std::string nodePath() override {
		return _name;
	}

	FutureMaybe<smarter::shared_ptr<File, FileHandle>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(_lane, std::move(mount), std::move(link), semantic_flags);
	}

	FutureMaybe<std::shared_ptr<FsLink>> mount() override {
		return mountExternalDevice(_lane);
	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("SUBSYSTEM", "block");
	}

private:
	std::string _name;
	helix::UniqueLane _lane;
};

} // anonymous namepsace

async::detached run() {
	sysfsSubsystem = new drvcore::ClassSubsystem{"block"};

	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("unix.devtype", "block"),
		mbus::EqualsFilter("unix.blocktype", "disk")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		constexpr const char *alphabet = "abcdefghijklmnopqrstuvwxyz";

		auto id = entity.getId();
		int diskId = diskAllocator.allocate();
		assert(static_cast<size_t>(diskId) < sizeof(alphabet));
		diskNames.emplace(id, alphabet[diskId]);
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));

	filter = mbus::Conjunction({
		mbus::EqualsFilter("unix.devtype", "block"),
		mbus::EqualsFilter("unix.blocktype", "partition")
	});

	handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
		auto diskId = std::stoll(std::get<mbus::StringItem>(properties.at("unix.diskid")).value);
		auto diskName = diskNames.at(diskId);

		auto name = std::string("sd") + diskName + std::get<mbus::StringItem>(properties.at("unix.partid")).value;
		std::cout << "POSIX: Installing block device " << name << std::endl;

		auto parent_property = std::get<mbus::StringItem>(properties.at("drvcore.mbus-parent"));
		auto mbus_parent = std::stoi(parent_property.value);
		std::shared_ptr<drvcore::Device> parent_device;
		if (mbus_parent != -1)
			parent_device = pci_subsystem::getDeviceByMbus(mbus_parent);

		auto lane = helix::UniqueLane(co_await entity.bind());
		auto device = std::make_shared<Device>(VfsType::blockDevice,
				name,
				std::move(lane),
				parent_device);
		// We use 8 here, the major for SCSI devices and allocate minors sequentially.
		// Note that this is not really correct as the minor of a partition
		// depends on the minor of the whole device (see Linux devices.txt documentation).
		device->assignId({8, minorAllocator.allocate()});
		blockRegistry.install(device);
		drvcore::installDevice(device);
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

} // namespace block_subsystem
