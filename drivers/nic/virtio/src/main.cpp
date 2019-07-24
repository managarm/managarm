#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <iostream>
#include <memory>
#include <queue>
#include <vector>

#include <cofiber.hpp>
#include <hel.h>
#include <hel-syscalls.h>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/svrctl/server.hpp>

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
	}

	helix::globalQueue()->run();
}
