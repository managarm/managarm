
#include <string.h>
#include <iostream>

#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../device.hpp"
#include "../drvcore.hpp"
#include "../util.hpp"
#include "../vfs.hpp"
#include "pci.hpp"

namespace drm_subsystem {

namespace {

drvcore::ClassSubsystem *sysfsSubsystem;

id_allocator<uint32_t> minorAllocator;

struct Subsystem {
	Subsystem() {
		minorAllocator.use_range(0);
	}
} subsystem;

struct Device final : UnixDevice, drvcore::ClassDevice {
	Device(int index, helix::UniqueLane lane, std::shared_ptr<drvcore::Device> parent)
	: UnixDevice{VfsType::charDevice},
			drvcore::ClassDevice{sysfsSubsystem, std::move(parent),
					"card" + std::to_string(index), this},
			_index{index}, _lane{std::move(lane)} { }

	std::string nodePath() override {
		return "dri/card" + std::to_string(_index);
	}

	async::result<frg::expected<Error, smarter::shared_ptr<File, FileHandle>>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(_lane, std::move(mount), std::move(link), semantic_flags);
	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("SUBSYSTEM", "drm");
	}

	std::optional<std::string> getClassPath() override {
		return "drm";
	};

private:
	int _index;
	helix::UniqueLane _lane;
};

} // anonymous namepsace

async::detached run() {
	sysfsSubsystem = new drvcore::ClassSubsystem{"drm"};

	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("unix.subsystem", "drm")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
		int index = minorAllocator.allocate();
		std::cout << "POSIX: Installing DRM device "
				<< std::get<mbus::StringItem>(properties.at("unix.devname")).value << std::endl;
		auto parent_property = std::get<mbus::StringItem>(properties.at("drvcore.mbus-parent"));
		auto mbus_parent = std::stoi(parent_property.value);
		auto pci_parent = pci_subsystem::getDeviceByMbus(mbus_parent);
		assert(pci_parent);

		auto lane = helix::UniqueLane(co_await entity.bind());
		auto device = std::make_shared<Device>(index, std::move(lane), pci_parent);
		// The minor is only correct for card* devices but not for control* and render*.
		device->assignId({226, index});

		charRegistry.install(device);
		drvcore::installDevice(device);
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

} // namespace drm_subsystem
