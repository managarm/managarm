#include "protocols/hw/client.hpp"
#include "src/drvcore.hpp"

#include "power_supply.hpp"

namespace {

drvcore::ClassSubsystem *sysfsSubsystem;

struct TypeAttribute : sysfs::Attribute {
	TypeAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct StatusAttribute : sysfs::Attribute {
	StatusAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct CurrentNowAttribute : sysfs::Attribute {
	CurrentNowAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct PowerNowAttribute : sysfs::Attribute {
	PowerNowAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct EnergyNowAttribute : sysfs::Attribute {
	EnergyNowAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct EnergyFullAttribute : sysfs::Attribute {
	EnergyFullAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct EnergyFullDesignAttribute : sysfs::Attribute {
	EnergyFullDesignAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct VoltageNowAttribute : sysfs::Attribute {
	VoltageNowAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct VoltageMinDesignAttribute : sysfs::Attribute {
	VoltageMinDesignAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

TypeAttribute typeAttr{"type"};
StatusAttribute statusAttr{"status"};
CurrentNowAttribute currentNowAttr{"current_now"};
PowerNowAttribute powerNowAttr{"power_now"};
EnergyNowAttribute energyNowAttr{"energy_now"};
EnergyFullAttribute energyFullAttr{"energy_full"};
EnergyFullDesignAttribute energyFullDesignAttr{"energy_full_design"};
VoltageNowAttribute voltageNowAttr{"voltage_now"};
VoltageMinDesignAttribute voltageMinDesignAttr{"voltage_min_design"};

struct Device final : drvcore::ClassDevice {
	Device(drvcore::ClassSubsystem *subsystem, std::string name, protocols::hw::Device hwDevice,
		std::shared_ptr<drvcore::Device> parent)
		: drvcore::ClassDevice{subsystem, parent, std::move(name), nullptr},
		_hwDevice{std::move(hwDevice)} {
	}

	async::result<void> init() {
		co_await _hwDevice.getBatteryState(_state);

		co_return;
	}

	async::detached run() {
		while(true) {
			co_await _hwDevice.getBatteryState(_state, true);
		}
	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("SUBSYSTEM", "power_supply");
	}

	std::optional<std::string> getClassPath() override {
		return "power_supply";
	};

	protocols::hw::Device _hwDevice;
	protocols::hw::BatteryState _state;
};

std::unordered_map<mbus_ng::EntityId, std::shared_ptr<Device>> mbusMap;

async::result<frg::expected<Error, std::string>> TypeAttribute::show(sysfs::Object *) {
	co_return "Battery\n";
}

async::result<frg::expected<Error, std::string>> StatusAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	co_return device->_state.charging ? "Charging\n" : "Discharging\n";
}

async::result<frg::expected<Error, std::string>> CurrentNowAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	assert(device->_state.current_now);
	co_return std::format("{}\n", *device->_state.current_now);
}

async::result<frg::expected<Error, std::string>> PowerNowAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	assert(device->_state.power_now);
	co_return std::format("{}\n", *device->_state.power_now);
}

async::result<frg::expected<Error, std::string>> EnergyNowAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	assert(device->_state.energy_now);
	co_return std::format("{}\n", *device->_state.energy_now);
}

async::result<frg::expected<Error, std::string>> EnergyFullAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	assert(device->_state.energy_full);
	co_return std::format("{}\n", *device->_state.energy_full);
}

async::result<frg::expected<Error, std::string>> EnergyFullDesignAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	assert(device->_state.energy_full_design);
	co_return std::format("{}\n", *device->_state.energy_full_design);
}

async::result<frg::expected<Error, std::string>> VoltageNowAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	assert(device->_state.voltage_now);
	co_return std::format("{}\n", *device->_state.voltage_now);
}

async::result<frg::expected<Error, std::string>> VoltageMinDesignAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	assert(device->_state.voltage_min_design);
	co_return std::format("{}\n", *device->_state.voltage_min_design);
}

} // namespace

namespace power_supply_subsystem {

async::detached run() {
	sysfsSubsystem = new drvcore::ClassSubsystem{"power_supply"};

	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "power_supply"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			protocols::hw::Device hwDevice((co_await entity.getRemoteLane()).unwrap());

			auto parent_id = std::get<mbus_ng::StringItem>(event.properties.at("drvcore.mbus-parent")).value;
			auto parent_dev = drvcore::getMbusDevice(std::stoi(parent_id));
			auto power_type = std::get<mbus_ng::StringItem>(event.properties.at("power_supply.type")).value;
			auto id = std::get<mbus_ng::StringItem>(event.properties.at("power_supply.id")).value;

			if(power_type != "battery")
				continue;

			auto devname = std::format("BAT{}", id);
			auto dev = std::make_shared<Device>(sysfsSubsystem, devname, std::move(hwDevice), parent_dev);
			co_await dev->init();

			drvcore::installDevice(dev);

			dev->realizeAttribute(&typeAttr);
			dev->realizeAttribute(&statusAttr);
			if(dev->_state.current_now)
				dev->realizeAttribute(&currentNowAttr);
			if(dev->_state.power_now)
				dev->realizeAttribute(&powerNowAttr);
			if(dev->_state.energy_now)
				dev->realizeAttribute(&energyNowAttr);
			if(dev->_state.energy_full)
				dev->realizeAttribute(&energyFullAttr);
			if(dev->_state.energy_full_design)
				dev->realizeAttribute(&energyFullDesignAttr);
			if(dev->_state.voltage_now)
				dev->realizeAttribute(&voltageNowAttr);
			if(dev->_state.voltage_min_design)
				dev->realizeAttribute(&voltageMinDesignAttr);

			mbusMap.insert({entity.id(), dev});
			dev->run();
		}
	}
}

} // namespace power_supply_subsystem
