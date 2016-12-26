
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
#include <mbus.hpp>

#include "block.hpp"
#include "hw.pb.h"

// TODO: Support more than one device.
virtio::block::Device device;

COFIBER_ROUTINE(cofiber::no_future, bindDevice(mbus::Entity entity), ([=] {
	using M = helix::AwaitMechanism;

	auto lane = helix::UniqueLane(COFIBER_AWAIT entity.bind());

	// receive the device descriptor.
	helix::RecvInline<M> recv_resp;
	helix::PullDescriptor<M> pull_bar;
	helix::PullDescriptor<M> pull_irq;

	helix::submitAsync(lane, {
		helix::action(&recv_resp, kHelItemChain),
		helix::action(&pull_bar, kHelItemChain),
		helix::action(&pull_irq),
	}, helix::Dispatcher::global());

	COFIBER_AWAIT recv_resp.future();
	COFIBER_AWAIT pull_bar.future();
	COFIBER_AWAIT pull_irq.future();
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_bar.error());
	HEL_CHECK(pull_irq.error());

	managarm::hw::PciDevice resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());

	// run the UHCI driver.
	assert(resp.bars(0).io_type() == managarm::hw::IoType::PORT);
	assert(resp.bars(1).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(2).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(3).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(4).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(5).io_type() == managarm::hw::IoType::NONE);
	HEL_CHECK(helEnableIo(pull_bar.descriptor().getHandle()));

	std::cout << "Setting up the device" << std::endl;
	device.setupDevice(resp.bars(0).address(), pull_irq.descriptor());
}))

COFIBER_ROUTINE(cofiber::no_future, observeDevices(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-vendor", "1af4"),
		mbus::EqualsFilter("pci-device", "1001")
	});
	COFIBER_AWAIT root.linkObserver(std::move(filter),
			[] (mbus::AnyEvent event) {
		if(event.type() == typeid(mbus::AttachEvent)) {
			std::cout << "virtio: Detected block device" << std::endl;
			bindDevice(boost::get<mbus::AttachEvent>(event).getEntity());
		}else{
			throw std::runtime_error("Unexpected device class");
		}
	});
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting virtio-block driver\n");

	observeDevices();

	while(true)
		helix::Dispatcher::global().dispatch();
}

