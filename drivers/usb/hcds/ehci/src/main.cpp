
#include <assert.h>
#include <stdio.h>
#include <deque>
#include <experimental/optional>
#include <functional>
#include <iostream>
#include <memory>

#include <cofiber.hpp>
#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/memory.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/server.hpp>

#include "ehci.hpp"
#include "spec.hpp"

std::vector<std::shared_ptr<Controller>> globalControllers;

Controller::Controller(void *address, helix::UniqueIrq irq)
: _space(address), _irq(frigg::move(irq)) { 
	auto offset = _space.load(cap_regs::caplength);
	_operational = _space.subspace(offset);
	_numPorts = _operational.load(op_regs::hcsparams) & hcsparams::nPorts;
}

COFIBER_ROUTINE(async::result<void>, Controller::pollDevices(), ([=] {
	assert(!(_operational.load(op_regs::hcsparams) & hcsparams::portPower));
	
	while(true) {
		for(int i = 0; i < _numPorts; i++) {
			auto offset = _space.load(cap_regs::caplength);
			auto port_space = _space.subspace(offset + 0x44 + (4 * i));

			if(!(port_space.load(port_regs::sc) & portsc::connectChange))
				continue;
			port_space.store(port_regs::sc, portsc::connectChange(true));
			
			if(!(port_space.load(port_regs::sc) & portsc::connectStatus))
				std::runtime_error("EHCI device disconnected");
			
			if((port_space.load(port_regs::sc) & portsc::lineStatus) == 0x01)
				std::runtime_error("Device is low-speed");

			port_space.store(port_regs::sc, portsc::portReset(true));	
			// TODO: do not busy-wait.
			uint64_t start;
			HEL_CHECK(helGetClock(&start));
			while(true) {
				uint64_t ticks;
				HEL_CHECK(helGetClock(&ticks));
				if(ticks - start >= 50000000)
					break;
			}
			port_space.store(port_regs::sc, portsc::portReset(false));

			while(port_space.load(port_regs::sc) & portsc::portReset) {

			}
			
			if(!(port_space.load(port_regs::sc) & portsc::portStatus))
				std::runtime_error("Device is full-speed");

			std::cout << "High-speed device detected!" << std::endl;
		}
	}
}))

void Controller::initialize() {
	// Halt the controller.
	_operational.store(op_regs::usbcmd, usbcmd::run(false));
	while(!(_operational.load(op_regs::usbsts) & usbsts::hcHalted)) {
		// Wait until the controller has stopped.
	}

	// Reset the controller.
	_operational.store(op_regs::usbcmd, usbcmd::hcReset(true) | usbcmd::irqThreshold(0x08));
	while(_operational.load(op_regs::usbcmd) & usbcmd::hcReset) {
		// Wait until the reset is complete.
	}

	// Initialize controller.
	_operational.store(op_regs::usbintr, usbintr::transaction(true) 
			| usbintr::usbError(true) | usbintr::portChange(true) 
			| usbintr::hostError(true));
	_operational.store(op_regs::usbcmd, usbcmd::run(true) | usbcmd::irqThreshold(0x08));
	_operational.store(op_regs::configflag, 0x01);

	pollDevices();
}


COFIBER_ROUTINE(cofiber::no_future, bindDevice(mbus::Entity entity), ([=] {
	protocols::hw::Device device(COFIBER_AWAIT entity.bind());
	auto info = COFIBER_AWAIT device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = COFIBER_AWAIT device.accessBar(0);
	auto irq = COFIBER_AWAIT device.accessIrq();
	
	void *actual_pointer;
	HEL_CHECK(helMapMemory(bar.getHandle(), kHelNullHandle, nullptr,
			0, 0x1000, kHelMapReadWrite | kHelMapShareAtFork, &actual_pointer));

	auto controller = std::make_shared<Controller>(actual_pointer, std::move(irq));
	controller->initialize();

	globalControllers.push_back(std::move(controller));
}))

COFIBER_ROUTINE(cofiber::no_future, observeDevices(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-class", "0c"),
		mbus::EqualsFilter("pci-subclass", "03"),
		mbus::EqualsFilter("pci-interface", "20")
	});
	COFIBER_AWAIT root.linkObserver(std::move(filter),
			[] (mbus::AnyEvent event) {
		if(event.type() == typeid(mbus::AttachEvent)) {
			std::cout << "ehci: Detected device" << std::endl;
			bindDevice(boost::get<mbus::AttachEvent>(event).getEntity());
		}else{
			throw std::runtime_error("Unexpected event type");
		}
	});
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting EHCI driver\n");

	observeDevices();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}
 
