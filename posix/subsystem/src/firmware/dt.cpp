#include "dt.hpp"

#include "../drvcore.hpp"
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>

#include <print>

namespace {

struct Property final : sysfs::Attribute {
	Property(std::string name, std::vector<uint8_t> data)
	: sysfs::Attribute{std::move(name), false},
	  data_{std::move(data)} {}

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *) override {
		co_return std::string(data_.begin(), data_.end());
	}

	std::vector<std::string_view> parseStringList() {
		std::vector<std::string_view> list;

		size_t i = 0;
		while (i < data_.size()) {
			std::string_view sv{reinterpret_cast<const char *>(data_.data()) + i};
			i += sv.size() + 1;
			list.push_back(sv);
		}

		return list;
	}

private:
	std::vector<uint8_t> data_;
};

struct Node final : drvcore::Device {
	Node(
	    std::string name,
	    mbus_ng::EntityId mbusId,
	    protocols::hw::Device device,
	    std::shared_ptr<drvcore::Device> parent,
	    std::shared_ptr<sysfs::Object> parentDirectory
	)
	: drvcore::Device{parent, parentDirectory, std::move(name), nullptr},
	  device_{std::move(device)},
	  mbusId_{mbusId} {}

	void composeUevent(drvcore::UeventProperties &ue) override {
		// TODO: Add missing properties
		auto compatibleProp = findProperty("compatible");
		int compatibleCount = 0;

		if (compatibleProp) {
			for (auto &compatible : compatibleProp->parseStringList()) {
				ue.set(std::format("OF_COMPATIBLE_{}", compatibleCount), std::string{compatible});
				++compatibleCount;
			}
		}

		ue.set("OF_COMPATIBLE_N", std::to_string(compatibleCount));
		ue.set("MBUS_ID", std::to_string(mbusId_));
	}

	protocols::hw::Device &getDevice() { return device_; }

	async::result<void> addProperties() {
		auto properties = co_await device_.getDtProperties();

		for (auto &property : properties) {
			auto propObject =
			    std::make_unique<Property>(std::move(property.first), property.second.data());
			properties_.push_back(std::move(propObject));
		}
	}

	void publish() {
		for (auto &property : properties_) {
			realizeAttribute(property.get());
		}
	}

	Property *findProperty(std::string_view name) {
		for (auto &prop : properties_) {
			if (prop->name() == name) {
				return prop.get();
			}
		}

		return nullptr;
	}

private:
	protocols::hw::Device device_;
	mbus_ng::EntityId mbusId_;
	std::vector<std::unique_ptr<Property>> properties_;
};

std::shared_ptr<sysfs::Object> deviceTreeObject;
std::shared_ptr<sysfs::Object> deviceTreeBaseObject;

async::detached bind(mbus_ng::Entity entity, mbus_ng::Properties properties) {
	protocols::hw::Device hwDevice((co_await entity.getRemoteLane()).unwrap());
	auto path = co_await hwDevice.getDtPath();

	std::println(std::cout, "POSIX: Installing DT device {} (mbus ID: {})", path, entity.id());

	std::shared_ptr<drvcore::Device> parentObj;
	std::shared_ptr<sysfs::Object> parentDir;

	auto slashPos = path.find_last_of('/');
	std::string name;
	if (slashPos == 0) {
		name = path.substr(1);

		parentDir = deviceTreeBaseObject;
	} else {
		name = path.substr(slashPos + 1);

		auto parentId =
		    std::stoi(std::get<mbus_ng::StringItem>(properties["drvcore.mbus-parent"]).value);
		while (!parentObj) {
			auto ret = drvcore::getMbusDevice(parentId);
			if (ret) {
				parentObj = std::static_pointer_cast<drvcore::Device>(ret);
				break;
			}
			co_await drvcore::mbusMapUpdate.async_wait();
		}

		parentDir = parentObj;
	}

	auto node = std::make_shared<Node>(
	    std::move(name),
	    entity.id(),
	    std::move(hwDevice),
	    std::move(parentObj),
	    std::move(parentDir)
	);

	co_await node->addProperties();

	drvcore::installDevice(node);

	// TODO: Call realizeAttribute *before* installing the device.
	node->publish();

	drvcore::registerMbusDevice(entity.id(), node);
}

} // namespace

namespace firmware_dt {

async::detached run() {
	deviceTreeObject = std::make_shared<sysfs::Object>(drvcore::firmwareObject(), "devicetree");
	deviceTreeObject->addObject();
	deviceTreeBaseObject = std::make_shared<sysfs::Object>(deviceTreeObject, "base");
	deviceTreeBaseObject->addObject();

	auto filter = mbus_ng::Conjunction({
	    mbus_ng::EqualsFilter{"unix.subsystem", "dt"},
	});

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);

	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			bind(std::move(entity), std::move(event.properties));
		}
	}
}

} // namespace firmware_dt
