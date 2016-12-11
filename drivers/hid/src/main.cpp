
#include <iostream>

#include <stdio.h>

#include <helix/await.hpp>
#include <cofiber.hpp>
#include <mbus.hpp>
#include <protocols/usb/client.hpp>

COFIBER_ROUTINE(cofiber::no_future, bindDevice(mbus::Entity entity), ([=] {
	using M = helix::AwaitMechanism;

	auto lane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
	auto device = protocols::usb::connect(std::move(lane));
	COFIBER_AWAIT device.configurationDescriptor();
}))

COFIBER_ROUTINE(cofiber::no_future, observeDevices(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("usb.type", "device"),
		mbus::EqualsFilter("usb.class", "00")
	});
	COFIBER_AWAIT root.linkObserver(std::move(filter),
			[] (mbus::AnyEvent event) {
		if(event.type() == typeid(mbus::AttachEvent)) {
			std::cout << "uhci: Detected hid-device" << std::endl;
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
	printf("Starting uhci (usb-)driver\n");

	observeDevices();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}

