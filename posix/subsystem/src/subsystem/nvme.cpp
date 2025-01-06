#include <core/id-allocator.hpp>
#include <protocols/mbus/client.hpp>

#include "../drvcore.hpp"

namespace nvme_subsystem {

namespace {

drvcore::ClassSubsystem *nvmeSubsystem;
drvcore::ClassSubsystem *fabricsSubsystem;
drvcore::ClassSubsystem *subsystemSubsystem;

struct Subsystem final : drvcore::ClassDevice {
	Subsystem(drvcore::ClassSubsystem *subsystem, size_t num)
		: drvcore::ClassDevice{subsystem, nullptr, std::format("nvme-subsys{}", num), nullptr} {

	}

	void composeUevent(drvcore::UeventProperties &) override {
	}

	std::optional<std::string> getClassPath() override {
		return "nvme-subsystem";
	};
};

struct Controller final : drvcore::ClassDevice {
	Controller(drvcore::ClassSubsystem *subsystem, size_t num, std::shared_ptr<Device> parent,
		std::shared_ptr<Device> subsys, std::string address, std::string transport, std::string serial, std::string model, std::string fw_rev)
		: drvcore::ClassDevice{subsystem, parent, std::format("nvme{}", num), nullptr},
		subsystem{subsys}, address{address}, transport{transport}, serial{serial}, model{model}, fw_rev{fw_rev} {

	}

	void composeUevent(drvcore::UeventProperties &) override {
	}

	std::optional<std::string> getClassPath() override {
		return "nvme";
	};

	std::shared_ptr<Device> subsystem;
	std::string address;
	std::string transport;
	std::string serial;
	std::string model;
	std::string fw_rev;
};

struct Namespace final : drvcore::Device {
	Namespace(std::shared_ptr<Device> parent, size_t nsid)
		: drvcore::Device{parent, std::format("{}n{}", parent->name(), nsid), nullptr}, nsid_{nsid} {

	}

	void composeUevent(drvcore::UeventProperties &) override {
	}

	size_t nsid() const {
		return nsid_;
	}

	std::shared_ptr<sysfs::Object> queue;

private:
	size_t nsid_;
};

struct ControllerSubsysNqnAttribute : sysfs::Attribute {
	ControllerSubsysNqnAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct SubsysNqnAttribute : sysfs::Attribute {
	SubsysNqnAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct SubsysTypeAttribute : sysfs::Attribute {
	SubsysTypeAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct IoPolicyAttribute : sysfs::Attribute {
	IoPolicyAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct TransportAttribute : sysfs::Attribute {
	TransportAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct AddressAttribute : sysfs::Attribute {
	AddressAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct StateAttribute : sysfs::Attribute {
	StateAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct CntlIdAttribute : sysfs::Attribute {
	CntlIdAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct NumaNodeAttribute : sysfs::Attribute {
	NumaNodeAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct SerialAttribute : sysfs::Attribute {
	SerialAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct ModelAttribute : sysfs::Attribute {
	ModelAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct FwRevAttribute : sysfs::Attribute {
	FwRevAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct CntrlTypeAttribute : sysfs::Attribute {
	CntrlTypeAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct NsidAttribute : sysfs::Attribute {
	NsidAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct NsSizeAttribute : sysfs::Attribute {
	NsSizeAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

struct QueueLogicalBlocksizeAttribute : sysfs::Attribute {
	QueueLogicalBlocksizeAttribute(std::string name)
	: sysfs::Attribute{std::move(name), false} { }

	async::result<frg::expected<Error, std::string>> show(sysfs::Object *object) override;
};

async::result<frg::expected<Error, std::string>> ControllerSubsysNqnAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Controller *>(object);
	co_return std::format("nqn.2014-08.org.nvmexpress:nvm-subsystem:{}\n", device->subsystem->name());
}

async::result<frg::expected<Error, std::string>> SubsysNqnAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Subsystem *>(object);
	co_return std::format("nqn.2014-08.org.nvmexpress:nvm-subsystem:{}\n", device->name());
}

async::result<frg::expected<Error, std::string>> SubsysTypeAttribute::show(sysfs::Object *) {
	co_return "nvm\n";
}

async::result<frg::expected<Error, std::string>> IoPolicyAttribute::show(sysfs::Object *) {
	co_return "numa\n";
}

async::result<frg::expected<Error, std::string>> TransportAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Controller *>(object);
	co_return device->transport + "\n";
}

async::result<frg::expected<Error, std::string>> AddressAttribute::show(sysfs::Object *object) {
	auto c = static_cast<Controller *>(object);
	co_return std::format("{}\n", c->address);
}

async::result<frg::expected<Error, std::string>> StateAttribute::show(sysfs::Object *) {
	co_return "live\n";
}

async::result<frg::expected<Error, std::string>> CntlIdAttribute::show(sysfs::Object *) {
	co_return "2\n";
}

async::result<frg::expected<Error, std::string>> NumaNodeAttribute::show(sysfs::Object *) {
	co_return "-1\n";
}

async::result<frg::expected<Error, std::string>> SerialAttribute::show(sysfs::Object *object) {
	auto c = static_cast<Controller *>(object);
	co_return c->serial + "\n";
}

async::result<frg::expected<Error, std::string>> ModelAttribute::show(sysfs::Object *object) {
	auto c = static_cast<Controller *>(object);
	co_return c->model + "\n";
}

async::result<frg::expected<Error, std::string>> FwRevAttribute::show(sysfs::Object *object) {
	auto c = static_cast<Controller *>(object);
	co_return c->fw_rev + "\n";
}

async::result<frg::expected<Error, std::string>> CntrlTypeAttribute::show(sysfs::Object *) {
	co_return "io\n";
}

async::result<frg::expected<Error, std::string>> NsidAttribute::show(sysfs::Object *object) {
	auto ns = static_cast<Namespace *>(object);
	co_return std::format("{}\n", ns->nsid());
}

async::result<frg::expected<Error, std::string>> NsSizeAttribute::show(sysfs::Object *) {
	co_return "0\n";
}

async::result<frg::expected<Error, std::string>> QueueLogicalBlocksizeAttribute::show(sysfs::Object *) {
	co_return "512\n";
}

ControllerSubsysNqnAttribute ctrlSubsysNqnAttr{"subsysnqn"};
SubsysNqnAttribute subsysNqnAttr{"subsysnqn"};
SubsysTypeAttribute subsysTypeAttr{"subsystype"};
IoPolicyAttribute ioPolicyAttr{"iopolicy"};
TransportAttribute transportAttr{"transport"};
AddressAttribute addressAttr{"address"};
StateAttribute stateAttr{"state"};
CntlIdAttribute cntlIdAttr{"cntlid"};
CntrlTypeAttribute cntrlTypeAttr{"cntrltype"};
NumaNodeAttribute numaNodeAttr{"numa_node"};
SerialAttribute serialAttr{"serial"};
ModelAttribute modelAttr{"model"};
FwRevAttribute fwRevAttr{"firmware_rev"};

NsidAttribute nsidAttr{"nsid"};
NsSizeAttribute nsSizeAttr{"size"};
QueueLogicalBlocksizeAttribute lbaSizeAttr{"logical_block_size"};

struct FabricsCtl final : drvcore::ClassDevice {
	FabricsCtl() : drvcore::ClassDevice(fabricsSubsystem, nullptr, "ctl", nullptr) {}

	void composeUevent(drvcore::UeventProperties &) override {
	}
};

std::shared_ptr<FabricsCtl> fabricsSubsystemCtl;

} // namespace

async::detached run() {
	fabricsSubsystem = new drvcore::ClassSubsystem{"nvme-fabrics"};
	drvcore::virtualDeviceParent()->createSymlink("nvme-fabrics", fabricsSubsystem->object());

	fabricsSubsystemCtl = std::make_shared<FabricsCtl>();
	drvcore::installDevice(fabricsSubsystemCtl);

	size_t subsystems = 0;
	size_t controllers = 0;

	nvmeSubsystem = new drvcore::ClassSubsystem{"nvme"};

	subsystemSubsystem = new drvcore::ClassSubsystem{"nvme-subsystem"};
	drvcore::virtualDeviceParent()->createSymlink("nvme-subsystem", subsystemSubsystem->object());

	auto filter = mbus_ng::Disjunction{{
		mbus_ng::EqualsFilter{"class", "nvme-subsystem"},
		mbus_ng::EqualsFilter{"class", "nvme-controller"},
		mbus_ng::EqualsFilter{"class", "nvme-namespace"},
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			auto type = std::get<mbus_ng::StringItem>(event.properties.at("class")).value;

			if(type == "nvme-subsystem") {
				auto subsys = std::make_shared<Subsystem>(subsystemSubsystem, subsystems++);
				drvcore::installDevice(subsys);

				subsys->realizeAttribute(&subsysNqnAttr);
				subsys->realizeAttribute(&subsysTypeAttr);
				subsys->realizeAttribute(&ioPolicyAttr);

				std::cout << std::format("posix: installed {} (mbus ID {})", subsys->name(), entity.id()) << std::endl;
				drvcore::registerMbusDevice(entity.id(), std::move(subsys));
			} else if(type == "nvme-controller") {
				auto subsys = ({
					auto parent_id = std::get<mbus_ng::StringItem>(event.properties.at("nvme.subsystem")).value;
					drvcore::getMbusDevice(mbus_ng::EntityId{std::stoi(parent_id)});
				});
				assert(subsys);

				auto parent = ({
					auto parent_id = std::get<mbus_ng::StringItem>(event.properties.at("drvcore.mbus-parent")).value;
					drvcore::getMbusDevice(mbus_ng::EntityId{std::stoi(parent_id)});
				});

				auto nvme_device_address = std::get<mbus_ng::StringItem>(event.properties.at("nvme.address")).value;
				auto nvme_transport = std::get<mbus_ng::StringItem>(event.properties.at("nvme.transport")).value;
				auto nvme_model = std::get<mbus_ng::StringItem>(event.properties.at("nvme.model")).value;
				auto nvme_serial = std::get<mbus_ng::StringItem>(event.properties.at("nvme.serial")).value;
				auto nvme_fw_rev = std::get<mbus_ng::StringItem>(event.properties.at("nvme.fw-rev")).value;

				if(!parent && nvme_transport == "tcp")
					parent = fabricsSubsystemCtl;

				auto controller = std::make_shared<Controller>(nvmeSubsystem, controllers++, parent, subsys, nvme_device_address, nvme_transport, nvme_serial, nvme_model, nvme_fw_rev);

				assert(controller);
				drvcore::installDevice(controller);

				subsys->createSymlink(controller->name(), controller);
				controller->realizeAttribute(&ctrlSubsysNqnAttr);
				controller->realizeAttribute(&transportAttr);
				controller->realizeAttribute(&addressAttr);
				controller->realizeAttribute(&stateAttr);
				controller->realizeAttribute(&cntlIdAttr);
				controller->realizeAttribute(&cntrlTypeAttr);
				controller->realizeAttribute(&numaNodeAttr);
				controller->realizeAttribute(&serialAttr);
				controller->realizeAttribute(&modelAttr);
				controller->realizeAttribute(&fwRevAttr);

				std::cout << std::format("posix: installed {} (mbus ID {})", controller->name(), entity.id()) << std::endl;
				drvcore::registerMbusDevice(entity.id(), std::move(controller));
			} else if(type == "nvme-namespace") {
				auto parent = ({
					auto parent_id = std::get<mbus_ng::StringItem>(event.properties.at("drvcore.mbus-parent")).value;
					drvcore::getMbusDevice(mbus_ng::EntityId{std::stoi(parent_id)});
				});
				assert(parent);

				auto nsid = std::stoi(std::get<mbus_ng::StringItem>(event.properties.at("nvme.nsid")).value);
				auto ns = std::make_shared<Namespace>(parent, nsid);
				drvcore::installDevice(ns);

				ns->realizeAttribute(&nsidAttr);
				ns->realizeAttribute(&nsSizeAttr);
				ns->queue = std::make_shared<sysfs::Object>(ns, "queue");
				ns->queue->addObject();
				ns->queue->realizeAttribute(&lbaSizeAttr);

				std::cout << std::format("posix: installed {} (mbus ID {})", ns->name(), entity.id()) << std::endl;
				drvcore::registerMbusDevice(entity.id(), std::move(ns));
			} else {
				std::cout << std::format("posix: unsupported NVMe device type '{}' (mbus ID {})", type, entity.id()) << std::endl;
			}
		}
	}

	co_return;
}

} // namespace nvme_subsystem
