
#include <deque>
#include <experimental/optional>
#include <iostream>

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/await.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/client.hpp>

#include "storage.hpp"

COFIBER_ROUTINE(cofiber::no_future, runStorageDevice(Device device), [=] () {
	printf("entered runStorageDevice\n");
	auto descriptor = COFIBER_AWAIT device.configurationDescriptor();

	std::experimental::optional<int> config_number;
	std::experimental::optional<int> intf_number;
	std::experimental::optional<int> in_endp_number;
	std::experimental::optional<int> out_endp_number;

	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		if(type == kDescriptorConfig) {
			assert(!config_number);
			config_number = info.configNumber.value();
			
			auto desc = (ConfigDescriptor *)p;
			printf("Config Descriptor: \n");
			printf("    value: %i\n", desc->configValue);
		}else if(type == kDescriptorInterface) {
			assert(!intf_number);
			intf_number = info.interfaceNumber.value();
			
			auto desc = (InterfaceDescriptor *)p;
			printf("Interface Descriptor: \n");
			printf("    class: %i\n", desc->interfaceClass);
			printf("    sub class: %i\n", desc->interfaceSubClass);
			printf("    protocoll: %i\n", desc->interfaceProtocoll);
			
			assert(desc->interfaceClass == 0x08);
			assert(desc->interfaceProtocoll == 0x50);
		}else if(type == kDescriptorEndpoint) {
			if(info.endpointIn.value()) {
				in_endp_number = info.endpointNumber.value();
			}else if(!info.endpointIn.value()) {
				out_endp_number = info.endpointNumber.value();
			}else {
				throw std::runtime_error("Illegal endpoint!\n");
			}
		}else{
			printf("Unexpected descriptor type: %d!\n", type);
		}
	});

	auto config = COFIBER_AWAIT device.useConfiguration(config_number.value());
	auto intf = COFIBER_AWAIT config.useInterface(intf_number.value(), 0);
	auto endp_in = COFIBER_AWAIT(intf.getEndpoint(PipeType::in, in_endp_number.value()));
	auto endp_out = COFIBER_AWAIT(intf.getEndpoint(PipeType::out, out_endp_number.value()));

	scsi::Read6 read;
	read.opCode = 0x08;
	read.lba[0] = 0x00;
	read.lba[1] = 0x00;
	read.lba[2] = 0x00;
	read.transferLength = 1;
	read.control = 0;

	CommandBlockWrapper cbw;
	cbw.signature = Signatures::kSignCbw;
	cbw.tag = 1;
	cbw.transferLength = 512;
	cbw.flags = 0x80;
	cbw.lun = 0;
	cbw.cmdLength = sizeof(scsi::Read6);
	memcpy(cbw.cmdData, &read, sizeof(scsi::Read6));

	COFIBER_AWAIT endp_out.transfer(BulkTransfer(XferFlags::kXferToDevice, &cbw, sizeof(CommandBlockWrapper)));

	uint8_t data[512];
	COFIBER_AWAIT endp_in.transfer(BulkTransfer(XferFlags::kXferToHost, data, 512));

	CommandStatusWrapper csw;
	COFIBER_AWAIT endp_in.transfer(BulkTransfer(XferFlags::kXferToHost,
			&csw, sizeof(CommandStatusWrapper)));

	assert(csw.signature == Signatures::kSignCsw);
	std::cout << "Bulk read complete. Data: " << (int)data[0] << " " << (int)data[1] << std::endl;
})

COFIBER_ROUTINE(cofiber::no_future, bindDevice(mbus::Entity entity), ([=] {
	auto lane = helix::UniqueLane(COFIBER_AWAIT entity.bind());
	auto device = protocols::usb::connect(std::move(lane));
	runStorageDevice(device);
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
			std::cout << "uhci: Detected storage-device" << std::endl;
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
	printf("Starting storage (usb-)driver\n");

	observeDevices();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}


