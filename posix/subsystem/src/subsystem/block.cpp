
#include <string.h>
#include <iostream>
#include <string_view>
#include <linux/fs.h>

#include <protocols/mbus/client.hpp>

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
			std::shared_ptr<drvcore::Device> parent, size_t size)
	: UnixDevice{type},
			drvcore::BlockDevice{sysfsSubsystem, std::move(parent), name,
					this},
			_name{std::move(name)}, _lane{std::move(lane)}, _size{size} { }

	std::string nodePath() override {
		return _name;
	}

	size_t size() {
		return _size;
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(_lane, std::move(mount), std::move(link), semantic_flags);
	}

	FutureMaybe<std::shared_ptr<FsLink>> mount() override {
		return mountExternalDevice(_lane);
	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		std::pair<int, int> dev = getId();
		ue.set("SUBSYSTEM", "block");
		ue.set("MAJOR", std::to_string(dev.first));
		ue.set("MINOR", std::to_string(dev.second));
	}

private:
	std::string _name;
	helix::UniqueLane _lane;

	size_t _size;
};

} // anonymous namepsace

struct ReadOnlyAttribute : sysfs::Attribute {
	ReadOnlyAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct DevAttribute : sysfs::Attribute {
	DevAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct SizeAttribute : sysfs::Attribute {
	SizeAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

ReadOnlyAttribute roAttr{"ro"};
DevAttribute devAttr{"dev"};
SizeAttribute sizeAttr{"size"};

async::result<frg::expected<Error, std::string>> ReadOnlyAttribute::show(sysfs::Object *object) {
	(void) object;
	// The format is 0\n\0.
	// Hardcode to zero as we don't support ro mounts yet.
	co_return "0\n";
}

async::result<frg::expected<Error, std::string>> DevAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	auto dev = device->getId();
	// The format is 0:0\n\0.
	co_return std::to_string(dev.first) + ":" + std::to_string(dev.second) + "\n";
}

async::result<frg::expected<Error, std::string>> SizeAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return std::to_string(device->size() / 512) + "\n";
}

async::detached observePartitions() {
	auto filter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter{"unix.devtype", "block"},
		mbus_ng::EqualsFilter{"unix.blocktype", "partition"}
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			auto &properties = event.properties;

			auto diskId = std::stoll(std::get<mbus_ng::StringItem>(properties.at("unix.diskid")).value);
			auto diskName = diskNames.at(diskId);

			auto name = std::string("sd") + diskName + std::get<mbus_ng::StringItem>(properties.at("unix.partid")).value;
			std::cout << "POSIX: Installing block device " << name << std::endl;

			auto parent_property = std::get<mbus_ng::StringItem>(properties.at("drvcore.mbus-parent"));
			auto mbus_parent = std::stoi(parent_property.value);
			std::shared_ptr<drvcore::Device> parent_device;
			if (mbus_parent != -1) {
				parent_device = pci_subsystem::getDeviceByMbus(mbus_parent);
				assert(parent_device);
			}

			auto lane = (co_await entity.getRemoteLane()).unwrap();

			managarm::fs::GenericIoctlRequest req;
			req.set_command(BLKGETSIZE64);

			auto ser = req.SerializeAsString();
			auto [offer, send_req, recv_resp] = co_await helix_ng::exchangeMsgs(lane,
					helix_ng::offer(
						helix_ng::sendBuffer(ser.data(), ser.size()),
						helix_ng::recvInline()
					)
			);
			HEL_CHECK(offer.error());
			HEL_CHECK(send_req.error());
			HEL_CHECK(recv_resp.error());

			managarm::fs::GenericIoctlReply resp;
			resp.ParseFromArray(recv_resp.data(), recv_resp.length());
			recv_resp.reset();
			assert(resp.error() == managarm::fs::Errors::SUCCESS);

			size_t size = resp.size();

			auto device = std::make_shared<Device>(VfsType::blockDevice,
					name,
					std::move(lane),
					parent_device,
					size);
			// We use 8 here, the major for SCSI devices and allocate minors sequentially.
			// Note that this is not really correct as the minor of a partition
			// depends on the minor of the whole device (see Linux devices.txt documentation).
			device->assignId({8, minorAllocator.allocate()});
			blockRegistry.install(device);
			drvcore::installDevice(device);

			// TODO: Call realizeAttribute *before* installing the device.
			device->realizeAttribute(&roAttr);
			device->realizeAttribute(&devAttr);
			device->realizeAttribute(&sizeAttr);

		}
	}
}

async::detached run() {
	sysfsSubsystem = new drvcore::ClassSubsystem{"block"};

	observePartitions();

	auto filter = mbus_ng::Conjunction({
		mbus_ng::EqualsFilter{"unix.devtype", "block"},
		mbus_ng::EqualsFilter{"unix.blocktype", "disk"}
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			constexpr std::string_view alphabet{"abcdefghijklmnopqrstuvwxyz"};

			int diskId = diskAllocator.allocate();
			assert(static_cast<size_t>(diskId) < alphabet.size());
			diskNames.emplace(event.id, alphabet[diskId]);
		}
	}
}

} // namespace block_subsystem
