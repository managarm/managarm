#pragma once

#include "src/drvcore.hpp"

namespace net_subsystem {

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

}
