
#include <string.h>
#include <iostream>
#include <sstream>

#include <cofiber.hpp>
#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../device.hpp"
#include "../drvcore.hpp"
#include "../util.hpp"
#include "../vfs.hpp"

namespace input_subsystem {

namespace {

id_allocator<uint32_t> evdevAllocator;

struct Subsystem {
	Subsystem() {
		evdevAllocator.use_range(64);
	}
} subsystem;

struct Device : UnixDevice {
	Device(VfsType type, std::string name, helix::UniqueLane lane)
	: UnixDevice{type},
			_name{std::move(name)}, _lane{std::move(lane)} { }

	std::string getName() override {
		return _name;
	}
	
	FutureMaybe<smarter::shared_ptr<File, FileHandle>> open(std::shared_ptr<FsLink> link,
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
		mbus::EqualsFilter("unix.subsystem", "input")
	});
	
	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		std::cout << "POSIX: Installing input device "
				<< std::get<mbus::StringItem>(properties.at("unix.devname")).value << std::endl;

		auto lane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
		auto device = std::make_shared<Device>(VfsType::charDevice,
				std::get<mbus::StringItem>(properties.at("unix.devname")).value,
				std::move(lane));
		device->assignId({13, evdevAllocator.allocate()});
		charRegistry.install(device);

		std::stringstream s;
		s << "add@/devices/event0" << '\0';
		s << "ACTION=add" << '\0';
		s << "DEVPATH=/devices/event0" << '\0';
		s << "SUBSYSTEM=input" << '\0';
		drvcore::emitHotplug(s.str());
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

} // namespace input_subsystem

