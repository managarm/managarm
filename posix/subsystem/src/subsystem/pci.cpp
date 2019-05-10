
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

std::vector<std::shared_ptr<drvcore::Device>> devices;

COFIBER_ROUTINE(cofiber::no_future, run(), ([] {
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

		auto device = std::make_shared<drvcore::Device>(nullptr, sysfs_name, nullptr);
		drvcore::installDevice(device);
		devices.push_back(std::move(device));

		std::stringstream s;
		s << "add@/devices/" << sysfs_name << '\0';
		s << "ACTION=add" << '\0';
		s << "DEVPATH=/devices/" << sysfs_name << '\0';
		s << "SUBSYSTEM=pci" << '\0';
		s << "SEQNUM=" << drvcore::makeHotplugSeqnum() << '\0';
		drvcore::emitHotplug(s.str());
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

} // namespace pci_subsystem

