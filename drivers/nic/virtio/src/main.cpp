#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <memory>
#include <queue>
#include <vector>

#include <async/result.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <helix/ipc.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/svrctl/server.hpp>
#include <protocols/fs/server.hpp>
#include "fs.pb.h"

#include "net.hpp"

// Maps mbus IDs to device objects
std::unordered_map<int64_t, std::shared_ptr<nic::virtio::Device>> baseDeviceMap;

async::result<void> doBind(mbus::Entity base_entity, virtio_core::DiscoverMode discover_mode) {
	protocols::hw::Device hw_device(co_await base_entity.bind());
	auto transport = co_await virtio_core::discover(std::move(hw_device), discover_mode);

	auto device = std::make_shared<nic::virtio::Device>(std::move(transport));
	baseDeviceMap.insert({base_entity.getId(), device});
	device->runDevice();
}

async::result<protocols::svrctl::Error> bindDevice(int64_t base_id) {
	std::cout << "nic-virtio: Binding to device " << base_id << std::endl;
	auto base_entity = co_await mbus::Instance::global().getEntity(base_id);

	// Do not bind to devices that are already bound to this driver.
	if(baseDeviceMap.find(base_entity.getId()) != baseDeviceMap.end())
		co_return protocols::svrctl::Error::success;

	// Make sure that we only bind to supported devices.
	auto properties = co_await base_entity.getProperties();
	if(auto vendor_str = std::get_if<mbus::StringItem>(&properties["pci-vendor"]);
			!vendor_str || vendor_str->value != "1af4")
		co_return protocols::svrctl::Error::deviceNotSupported;

	virtio_core::DiscoverMode discover_mode;
	if(auto device_str = std::get_if<mbus::StringItem>(&properties["pci-device"]); device_str) {
		if(device_str->value == "1000")
			discover_mode = virtio_core::DiscoverMode::transitional;
		else if(device_str->value == "1041")
			discover_mode = virtio_core::DiscoverMode::modernOnly;
		else
			co_return protocols::svrctl::Error::deviceNotSupported;
	}else{
		co_return protocols::svrctl::Error::deviceNotSupported;
	}

	co_await doBind(base_entity, discover_mode);
	co_return protocols::svrctl::Error::success;
}

namespace {
async::result<protocols::fs::ReadResult> noopRead(void *, const char *, void *buffer, size_t length) {
	memset(buffer, 0, length);
	co_return length;
}

async::result<void> noopWrite(void*, const char*, const void *, size_t) {
	co_return;
}
}

constexpr protocols::fs::FileOperations fileOps {
	.read  = &noopRead,
	.write = &noopWrite
};

async::detached serve(helix::UniqueLane lane) {
	while (true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		co_await header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());

		if (req.req_type() == managarm::fs::CntReqType::CREATE_SOCKET) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_socket;
			auto [local_lane, remote_lane] = helix::createStream();
			std::cout << "netserver: proto " << req.protocol()
				  << " type " << req.type() << std::endl;

			async::detach(protocols::fs::servePassthrough(
						std::move(local_lane), nullptr, &fileOps));

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_socket, remote_lane));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_socket.error());
		} else {
			helix::SendBuffer send_resp;
			std::cout << "netserver: received unknown request type: "
				<< req.req_type() << std::endl;

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}

async::detached advertise() {
	auto root = co_await mbus::Instance::global().getRoot();

	mbus::Properties descriptor {
		{"class", mbus::StringItem{"netserver"}}
	};

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		auto [local_lane, remote_lane] = helix::createStream();

		serve(std::move(local_lane));
		co_return std::move(remote_lane);
	});

	co_await root.createObject("netserver", descriptor, std::move(handler));
}

static constexpr protocols::svrctl::ControlOperations controlOps = {
	.bind = bindDevice
};

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("nic-virtio: Starting driver\n");

//	HEL_CHECK(helSetPriority(kHelThisThread, 3));

	{
		async::queue_scope scope{helix::globalQueue()};
		async::detach(protocols::svrctl::serveControl(&controlOps));
		advertise();
	}

	helix::globalQueue()->run();
}
