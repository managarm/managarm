
#include <string.h>
#include <iostream>
#include <sstream>

#include <cofiber.hpp>
#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../drvcore.hpp"
#include "../util.hpp"
#include "../vfs.hpp"
#include "fs.pb.h"

namespace pci_subsystem {

drvcore::BusSubsystem *sysfsSubsystem;

struct Device : drvcore::BusDevice {
	Device(std::string sysfs_name)
	: drvcore::BusDevice{sysfsSubsystem, std::move(sysfs_name), nullptr} { }

	void composeUevent(std::stringstream &ss) override {
		ss << "SUBSYSTEM=pci" << '\0';
	}
};

std::vector<std::shared_ptr<Device>> devices;

COFIBER_ROUTINE(cofiber::no_future, run(), ([] {
	sysfsSubsystem = new drvcore::BusSubsystem{"pci"};

	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("unix.subsystem", "pci")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) {
		std::string sysfs_name = "0000:" + std::get<mbus::StringItem>(properties["pci-bus"]).value
				+ ":" + std::get<mbus::StringItem>(properties["pci-slot"]).value
				+ "." + std::get<mbus::StringItem>(properties["pci-function"]).value;

		// TODO: Add bus/slot/function to this message.
		std::cout << "POSIX: Installing PCI device " << sysfs_name << std::endl;

		auto device = std::make_shared<Device>(sysfs_name);
		drvcore::installDevice(device);
		devices.push_back(std::move(device));
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

} // namespace pci_subsystem

