#include "src/drvcore.hpp"
#include "src/subsystem/usb/usb.hpp"
#include "net.hpp"

namespace net_subsystem {

namespace {

drvcore::ClassSubsystem *sysfsSubsystem;

struct Device final : drvcore::ClassDevice {
	Device(drvcore::ClassSubsystem *subsystem, std::string name, int ifindex, std::shared_ptr<drvcore::Device> parent)
		: drvcore::ClassDevice{subsystem, parent, name, nullptr},
		ifindex_{ifindex} {

	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("INTERFACE", name());
		ue.set("IFINDEX", std::to_string(ifindex_));
		ue.set("DEVTYPE", "wwan");
		ue.set("SUBSYSTEM", "net");
	}

	std::optional<std::string> getClassPath() override {
		return "net";
	};
private:
	int ifindex_;
};

std::unordered_map<mbus_ng::EntityId, std::shared_ptr<Device>> mbusMap;

} // namespace

async::detached run() {
	sysfsSubsystem = new drvcore::ClassSubsystem{"net"};

	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"unix.subsystem", "net"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			auto parent_id = std::get<mbus_ng::StringItem>(event.properties.at("drvcore.mbus-parent")).value;
			auto parent_dev = drvcore::getMbusDevice(std::stoi(parent_id));

			if(event.properties.contains("usb.parent-interface"))
				parent_dev = co_await usb_subsystem::getInterfaceDevice(parent_dev, event.properties);

			auto ifname = std::get<mbus_ng::StringItem>(event.properties["net.ifname"]).value;
			auto ifindex = std::get<mbus_ng::StringItem>(event.properties["net.ifindex"]).value;
			if(ifname.empty() || ifindex.empty()) {
				std::cout << std::format("posix: net class device is missing ifname or ifindex\n");
				continue;
			}

			auto net = std::make_shared<Device>(sysfsSubsystem, ifname, std::stoi(ifindex), parent_dev);
			drvcore::installDevice(net);
			mbusMap.insert({entity.id(), std::move(net)});
		}
	}
}

} // namespace net_subsystem
