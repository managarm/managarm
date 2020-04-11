
#include <linux/input.h>
#include <string.h>
#include <iostream>
#include <sstream>

#include <protocols/mbus/client.hpp>

#include "../common.hpp"
#include "../device.hpp"
#include "../drvcore.hpp"
#include "../util.hpp"
#include "../vfs.hpp"
#include "fs.pb.h"

namespace input_subsystem {

namespace {

drvcore::ClassSubsystem *sysfsSubsystem;

id_allocator<uint32_t> evdevAllocator;

struct Subsystem {
	Subsystem() {
		evdevAllocator.use_range(0);
	}
} subsystem;

struct CapabilityAttribute : sysfs::Attribute {
	CapabilityAttribute(std::string name, int index, size_t bits)
	: sysfs::Attribute{std::move(name), false}, _index{index}, _bits{bits} { }

	virtual async::result<std::string> show(sysfs::Object *object) override;

private:
	int _index;
	size_t _bits;
};

CapabilityAttribute evCapability{"ev", 0, EV_MAX};
CapabilityAttribute keyCapability{"key", EV_KEY, KEY_MAX};
CapabilityAttribute relCapability{"rel", EV_REL, REL_MAX};
CapabilityAttribute absCapability{"abs", EV_ABS, ABS_MAX};

struct Device final : UnixDevice, drvcore::ClassDevice {
	Device(VfsType type, int index, helix::UniqueLane lane)
	: UnixDevice{type},
			drvcore::ClassDevice{sysfsSubsystem, nullptr, "event" + std::to_string(index), this},
			_index{index}, _lane{std::move(lane)} { }

	std::string nodePath() override {
		return "input/event" + std::to_string(_index);
	}
	
	FutureMaybe<smarter::shared_ptr<File, FileHandle>>
	open(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link,
			SemanticFlags semantic_flags) override {
		return openExternalDevice(_lane, std::move(mount), std::move(link), semantic_flags);
	}

	void composeUevent(drvcore::UeventProperties &ue) override {
		ue.set("SUBSYSTEM", "input");
	}

private:
	int _index;
	helix::UniqueLane _lane;
};

async::result<std::string> CapabilityAttribute::show(sysfs::Object *object) {
	auto device = static_cast<Device *>(object);
	auto file = co_await device->open(nullptr, nullptr, 0);
	auto lane = file->getPassthroughLane();

	std::vector<uint64_t> buffer;
	buffer.resize((_bits + 63) & ~size_t(63));

	managarm::fs::CntRequest req;
	req.set_req_type(managarm::fs::CntReqType::PT_IOCTL);
	if(_index) {
		req.set_command(EVIOCGBIT(1, 0));
		req.set_input_type(_index);
	}else{
		req.set_command(EVIOCGBIT(0, 0));
	}
	req.set_size(buffer.size() * sizeof(uint64_t));

	auto ser = req.SerializeAsString();
	auto [req_error, offer, send_req, recv_resp, recv_data]
			= co_await helix_ng::exchangeMsgs(lane, helix::Dispatcher::global(),
		helix_ng::offer(
			helix_ng::sendBuffer(ser.data(), ser.size()),
			helix_ng::recvInline(),
			helix_ng::recvBuffer(buffer.data(), buffer.size() * sizeof(uint64_t))
		)
	);
	HEL_CHECK(req_error);
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(recv_data.error());
	
	managarm::fs::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::fs::Errors::SUCCESS);

	std::stringstream ss;
	ss << std::hex;
	bool suffix = false;
	for(auto it = buffer.rbegin(); it != buffer.rend(); it++) {
		if(!(*it) && !suffix)
			continue;
		if(it != buffer.rbegin())
			ss << ' ';
		ss << *it;
		suffix = true;
	}

	co_return ss.str();
}

} // anonymous namepsace

async::detached run() {
	sysfsSubsystem = new drvcore::ClassSubsystem{"input"};

	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("unix.subsystem", "input")
	});
	
	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties properties) -> async::detached {
		int index = evdevAllocator.allocate();
		std::cout << "POSIX: Installing input device input/event" << index << std::endl;

		auto lane = helix::UniqueLane(co_await entity.bind());
		auto device = std::make_shared<Device>(VfsType::charDevice,
				index, std::move(lane));
		device->assignId({13, 64 + index}); // evdev devices start at minor 64.

		charRegistry.install(device);
		drvcore::installDevice(device);

		// TODO: Do this before the device becomes visible in sysfs!
		auto link = device->directoryNode()->directMkdir("capabilities");
		auto caps = static_cast<sysfs::DirectoryNode *>(link->getTarget().get());
		caps->directMkattr(device.get(), &evCapability);
		caps->directMkattr(device.get(), &keyCapability);
		caps->directMkattr(device.get(), &relCapability);
		caps->directMkattr(device.get(), &absCapability);
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

} // namespace input_subsystem
