
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <functional>
#include <iostream>
#include <memory>

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <fafnir/dsl.hpp>
#include <helix/ipc.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/kernlet/compiler.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/server.hpp>

#include "spec.hpp"
#include "ehci.hpp"

static const bool logIrqs = false;
static const bool logPackets = false;
static const bool logSubmits = false;
static const bool logControllerEnumeration = false;
static const bool logDeviceEnumeration = false;

static const bool debugLinking = false;

namespace proto = protocols::usb;

std::vector<std::shared_ptr<Controller>> globalControllers;

// ----------------------------------------------------------------------------
// Memory management.
// ----------------------------------------------------------------------------

namespace {
	arch::contiguous_pool schedulePool;
}

// ----------------------------------------------------------------------------
// Pointer.
// ----------------------------------------------------------------------------

uint32_t physicalPointer(void *ptr) {
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(ptr, &physical));
	assert((physical & 0xFFFFFFFF) == physical);
	return physical;
}

uint32_t schedulePointer(void *ptr) {
	auto physical = physicalPointer(ptr);
	assert(!(physical & 0x1F));
	return physical;
}

// ----------------------------------------------------------------
// DeviceState
// ----------------------------------------------------------------

DeviceState::DeviceState(std::shared_ptr<Controller> controller, int device)
: _controller{std::move(controller)}, _device(device) { }

arch::dma_pool *DeviceState::setupPool() {
	return &schedulePool;
}

arch::dma_pool *DeviceState::bufferPool() {
	return &schedulePool;
}

async::result<frg::expected<proto::UsbError, std::string>> DeviceState::deviceDescriptor() {
	return _controller->deviceDescriptor(_device);
}

async::result<frg::expected<proto::UsbError, std::string>> DeviceState::configurationDescriptor(uint8_t configuration) {
	return _controller->configurationDescriptor(_device, configuration);
}

async::result<frg::expected<proto::UsbError, proto::Configuration>> DeviceState::useConfiguration(uint8_t index, uint8_t value) {
	FRG_CO_TRY(co_await _controller->useConfiguration(_device, value));
	co_return proto::Configuration{std::make_shared<ConfigurationState>(_controller,
				_device, index, value)};
}

async::result<frg::expected<proto::UsbError, size_t>> DeviceState::transfer(proto::ControlTransfer info) {
	return _controller->transfer(_device, 0, info);
}

// ----------------------------------------------------------------------------
// ConfigurationState
// ----------------------------------------------------------------------------

ConfigurationState::ConfigurationState(std::shared_ptr<Controller> controller,
		int device, uint8_t index, uint8_t value)
: _controller{std::move(controller)}, _device(device), _index(index), _value(value) {
}

async::result<frg::expected<proto::UsbError, proto::Interface>>
ConfigurationState::useInterface(int number, int alternative) {
	FRG_CO_TRY(co_await _controller->useInterface(_device, _index, _value, number, alternative));
	co_return proto::Interface{std::make_shared<InterfaceState>(_controller, _device, number)};
}

// ----------------------------------------------------------------------------
// InterfaceState
// ----------------------------------------------------------------------------

InterfaceState::InterfaceState(std::shared_ptr<Controller> controller,
		int device, int interface)
: proto::InterfaceData{interface}, _controller{std::move(controller)}, _device(device), _interface(interface) {
}

async::result<frg::expected<proto::UsbError, proto::Endpoint>>
InterfaceState::getEndpoint(proto::PipeType type, int number) {
	co_return proto::Endpoint{std::make_shared<EndpointState>(_controller,
			_device, type, number)};
}

// ----------------------------------------------------------------------------
// EndpointState
// ----------------------------------------------------------------------------

EndpointState::EndpointState(std::shared_ptr<Controller> controller,
		int device, proto::PipeType type, int endpoint)
: _controller{std::move(controller)}, _device(device), _type(type), _endpoint(endpoint) { }

async::result<frg::expected<proto::UsbError, size_t>> EndpointState::transfer(proto::ControlTransfer info) {
	(void)info;
	assert(!"FIXME: Implement this");
	__builtin_unreachable();
}

async::result<frg::expected<proto::UsbError, size_t>> EndpointState::transfer(proto::InterruptTransfer info) {
	return _controller->transfer(_device, _type, _endpoint, info);
}

async::result<frg::expected<proto::UsbError, size_t>> EndpointState::transfer(proto::BulkTransfer info) {
	return _controller->transfer(_device, _type, _endpoint, info);
}

// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

Controller::Controller(protocols::hw::Device hw_device, mbus_ng::EntityManager entity, helix::Mapping mapping,
		helix::UniqueDescriptor mmio, helix::UniqueIrq irq)
: _hwDevice{std::move(hw_device)}, _mapping{std::move(mapping)},
		_mmio{std::move(mmio)}, _irq{std::move(irq)},
		_space{_mapping.get()}, _enumerator{this},
		_entity{std::move(entity)} {
	auto offset = _space.load(cap_regs::caplength);
	_operational = _space.subspace(offset);
	_numPorts = _space.load(cap_regs::hcsparams) & hcsparams::nPorts;
	std::cout << "ehci: " << _numPorts  << " ports" << std::endl;

	if(_space.load(cap_regs::hccparams) & hccparams::extendedStructs)
		std::cout << "ehci: Controller uses 64-bit pointers" << std::endl;

	for(int i = 1; i < 128; i++) {
		_addressStack.push(i);
	}
}

async::detached Controller::initialize() {
	auto ext_pointer = _space.load(cap_regs::hccparams) & hccparams::extPointer;
	if(ext_pointer) {
		auto header = co_await _hwDevice.loadPciSpace(ext_pointer, 2);
		if(logControllerEnumeration)
			std::cout << "ehci: Extended capability: " << (header & 0xFF) << std::endl;

		assert((header & 0xFF) == 1);

		// TODO: We need a timeout here.
		if(!(co_await _hwDevice.loadPciSpace(ext_pointer + 3, 1))) {
			co_await _hwDevice.storePciSpace(ext_pointer + 3, 1, 1);
		}else{
			std::cout << "ehci: OS access to the EHCI is already requested" << std::endl;
		}

		if(logControllerEnumeration && (co_await _hwDevice.loadPciSpace(ext_pointer + 2, 1)))
			std::cout << "ehci: Controller is owned by the BIOS" << std::endl;

		co_await _hwDevice.storePciSpace(ext_pointer + 3, 1, 1);
		while(co_await _hwDevice.loadPciSpace(ext_pointer + 2, 1)) {
			// Do nothing while we wait for BIOS to release the EHCI.
		}
		if(logControllerEnumeration)
			std::cout << "ehci: Acquired OS <-> BIOS semaphore" << std::endl;

		assert(!(header & 0xFF00));
	}

	// Halt the controller.
	if(!(_operational.load(op_regs::usbsts) & usbsts::hcHalted)) {
		std::cout << "ehci: Taking over running controller" << std::endl;
		auto command = _operational.load(op_regs::usbcmd);
		_operational.store(op_regs::usbcmd, usbcmd::run(false)
				| usbcmd::irqThreshold(command & usbcmd::irqThreshold));
	}

	while(!(_operational.load(op_regs::usbsts) & usbsts::hcHalted)) {
		// Wait until the controller halts.
	}

	// Reset the controller.
	_operational.store(op_regs::usbcmd, usbcmd::hcReset(true) | usbcmd::irqThreshold(0x08));
	while(_operational.load(op_regs::usbcmd) & usbcmd::hcReset) {
		// Wait until the reset is complete.
	}
	if(logControllerEnumeration)
		std::cout << "ehci: Controller reset." << std::endl;

	// Initialize controller.
	_operational.store(op_regs::usbintr, usbintr::transaction(true)
			| usbintr::usbError(true) | usbintr::portChange(true)
			| usbintr::hostError(true));
	_operational.store(op_regs::usbcmd, usbcmd::run(true) | usbcmd::irqThreshold(0x08));
	_operational.store(op_regs::configflag, 0x01);

	_rootHub = std::make_shared<RootHub>(this);
	_enumerator.observeHub(_rootHub);

	_checkPorts();
	handleIrqs();
}

void Controller::_checkPorts() {
	assert(!(_space.load(cap_regs::hcsparams) & hcsparams::portPower));

	for(int i = 0; i < _numPorts; i++) {
		auto offset = _space.load(cap_regs::caplength);
		auto port_space = _space.subspace(offset + 0x44 + (4 * i));
		auto sc = port_space.load(port_regs::sc);
//		std::cout << "port " << i << " sc: " << static_cast<unsigned int>(sc) << std::endl;

		auto &port = _rootHub->port(i);

		if(sc & portsc::enableChange) {
			// EHCI specifies that enableChange is only set on port error.
			port_space.store(port_regs::sc, portsc::enableChange(true)
					| portsc::portOwner(sc & portsc::portOwner));
			if(!(sc & portsc::enableStatus)) {
				std::cout << "ehci: Port " << i << " disabled due to error" << std::endl;

				port.state.changes |= proto::HubStatus::enable;
				port.state.status &= ~proto::HubStatus::enable;
				port.pollEv.raise();
			}else{
				std::cout << "ehci: Spurious portsc::enableChange" << std::endl;
			}
		}

		if(sc & portsc::connectChange) {
			// TODO: Be careful to set the correct bits (e.g. suspend once we support it).
			port_space.store(port_regs::sc, portsc::connectChange(true)
					| portsc::portOwner(sc & portsc::portOwner));
			if(sc & portsc::connectStatus) {
				if((sc & portsc::lineStatus) == 1) { // K-state: Low-speed device
					if(logDeviceEnumeration)
						std::cout << "ehci: Device on port " << i << " is low-speed" << std::endl;
					// release the ownership of the port to the companion controller, as required by spec
					// see EHCI spec rev 1.0, p. 28
					port_space.store(port_regs::sc, portsc::portOwner(true));
				}else{
					if(logDeviceEnumeration)
						std::cout << "ehci: Connect on port " << i << std::endl;

					port.state.changes |= proto::HubStatus::connect;
					port.state.status |= proto::HubStatus::connect;
					port.pollEv.raise();
				}
			}else{
				if(logDeviceEnumeration)
					std::cout << "ehci: Disconnect on port " << i << std::endl;

				port.state.changes |= proto::HubStatus::connect;
				port.state.status &= ~proto::HubStatus::connect;
				port.pollEv.raise();
			}
		}
	}
}

async::result<void> Controller::enumerateDevice(std::shared_ptr<proto::Hub> hub, int port, proto::DeviceSpeed speed) {
	// TODO(qookie): Hub support
	assert(hub.get() == _rootHub.get());
	// Requires split TX when we have hub support
	assert(speed == proto::DeviceSpeed::highSpeed);

	// This queue will become the default control pipe of our new device.
	auto dma_obj = arch::dma_object<QueueHead>{&schedulePool};
	auto queue = new QueueEntity{std::move(dma_obj), 0, 0, proto::PipeType::control, 64};
	_linkAsync(queue);

	// Allocate an address for the device.
	assert(!_addressStack.empty());
	auto address = _addressStack.front();
	_addressStack.pop();

	if(logDeviceEnumeration)
		std::cout << "ehci: Setting device address" << std::endl;

	arch::dma_object<proto::SetupPacket> set_address{&schedulePool};
	set_address->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toDevice;
	set_address->request = proto::request_type::setAddress;
	set_address->value = address;
	set_address->index = 0;
	set_address->length = 0;

	(co_await _directTransfer(proto::ControlTransfer{proto::kXferToDevice,
			set_address, arch::dma_buffer_view{}}, queue, 0)).unwrap();

	queue->setAddress(address);

	// Enquire the maximum packet size of the default control pipe.
	if(logDeviceEnumeration)
		std::cout << "ehci: Getting device descriptor header" << std::endl;

	arch::dma_object<proto::SetupPacket> get_header{&schedulePool};
	get_header->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_header->request = proto::request_type::getDescriptor;
	get_header->value = proto::descriptor_type::device << 8;
	get_header->index = 0;
	get_header->length = 8;

	arch::dma_object<proto::DeviceDescriptor> descriptor{&schedulePool};
	(co_await _directTransfer(proto::ControlTransfer{proto::kXferToHost,
			get_header, descriptor.view_buffer().subview(0, 8)}, queue, 8)).unwrap();

	_activeDevices[address].controlStates[0].queueEntity = queue;
	_activeDevices[address].controlStates[0].maxPacketSize = descriptor->maxPacketSize;

	// Read the rest of the device descriptor.
	if(logDeviceEnumeration)
		std::cout << "ehci: Getting full device descriptor" << std::endl;

	arch::dma_object<proto::SetupPacket> get_descriptor{&schedulePool};
	get_descriptor->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_descriptor->request = proto::request_type::getDescriptor;
	get_descriptor->value = proto::descriptor_type::device << 8;
	get_descriptor->index = 0;
	get_descriptor->length = sizeof(proto::DeviceDescriptor);

	(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			get_descriptor, descriptor.view_buffer()})).unwrap();
	assert(descriptor->length == sizeof(proto::DeviceDescriptor));

	// TODO: Read configuration descriptor from the device.

	char class_code[3], sub_class[3], protocol[3];
	char vendor[5], product[5], release[5];
	snprintf(class_code, 3, "%.2x", descriptor->deviceClass);
	snprintf(sub_class, 3, "%.2x", descriptor->deviceSubclass);
	snprintf(protocol, 3, "%.2x", descriptor->deviceProtocol);
	snprintf(vendor, 5, "%.4x", descriptor->idVendor);
	snprintf(product, 5, "%.4x", descriptor->idProduct);
	snprintf(release, 5, "%.4x", descriptor->bcdDevice);

	char name[3];
	snprintf(name, 3, "%.2x", address);

	mbus_ng::Properties mbusDescriptor{
		{"usb.type", mbus_ng::StringItem{"device"}},
		{"usb.vendor", mbus_ng::StringItem{vendor}},
		{"usb.product", mbus_ng::StringItem{product}},
		{"usb.class", mbus_ng::StringItem{class_code}},
		{"usb.subclass", mbus_ng::StringItem{sub_class}},
		{"usb.protocol", mbus_ng::StringItem{protocol}},
		{"usb.release", mbus_ng::StringItem{release}},
		{"usb.hub_port", mbus_ng::StringItem{name}},
		{"usb.bus", mbus_ng::StringItem{std::to_string(_entity.id())}},
		{"usb.speed", mbus_ng::StringItem{"480"}},
		{"unix.subsystem", mbus_ng::StringItem{"usb"}},
	};

	auto usbEntity = (co_await mbus_ng::Instance::global().createEntity(
				"usb-ehci-dev-" + std::string{name}, mbusDescriptor)).unwrap();

	[] (auto self, auto address, mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			auto state = std::make_shared<DeviceState>(self->shared_from_this(), address);
			proto::serve(proto::Device{std::move(state)}, std::move(localLane));
		}
	}(this, address, std::move(usbEntity));
}

async::detached Controller::handleIrqs() {
	co_await connectKernletCompiler();

	std::vector<uint8_t> kernlet_program;
	fnr::emit_to(std::back_inserter(kernlet_program),
		// Load the USBSTS register.
		fnr::scope_push{} (
			fnr::intrin{"__mmio_read32", 2, 1} (
				fnr::binding{0}, // EHCI MMIO region (bound to slot 0).
				fnr::binding{1} // EHCI MMIO offset (bound to slot 1).
					 + fnr::literal{4} // Offset of USBSTS.
			) & fnr::literal{23} // USB transaction, error, port change and host error bits.
		),
		// Ack the IRQ iff one of the bits was set.
		fnr::check_if{},
			fnr::scope_get{0},
		fnr::then{},
			// Write back the interrupt bits to USBSTS to deassert the IRQ.
			fnr::intrin{"__mmio_write32", 3, 0} (
				fnr::binding{0}, // EHCI MMIO region (bound to slot 0).
				fnr::binding{1} // EHCI MMIO offset (bound to slot 1).
					+ fnr::literal{4}, // Offset of USBSTS.
				fnr::scope_get{0}
			),
			// Trigger the bitset event (bound to slot 2).
			fnr::intrin{"__trigger_bitset", 2, 0} (
				fnr::binding{2},
				fnr::scope_get{0}
			),
			fnr::scope_push{} ( fnr::literal{1} ),
		fnr::else_then{},
			fnr::scope_push{} ( fnr::literal{2} ),
		fnr::end{}
	);

	auto kernlet_object = co_await compile(kernlet_program.data(),
			kernlet_program.size(), {BindType::memoryView, BindType::offset,
			BindType::bitsetEvent});

	HelHandle event_handle;
	HEL_CHECK(helCreateBitsetEvent(&event_handle));
	helix::UniqueDescriptor event{event_handle};

	HelKernletData data[3];
	data[0].handle = _mmio.getHandle();
	data[1].handle = _mapping.offset() + _space.load(cap_regs::caplength);
	data[2].handle = event.getHandle();
	HelHandle bound_handle;
	HEL_CHECK(helBindKernlet(kernlet_object.getHandle(), data, 3, &bound_handle));
	HEL_CHECK(helAutomateIrq(_irq.getHandle(), 0, bound_handle));

	co_await _hwDevice.enableBusIrq();

	// Clear the IRQ in case it was pending while we attached the kernlet.
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckKick | kHelAckClear, 0));

	uint64_t sequence = 0;
	while(true) {
		if(logIrqs)
			std::cout << "ehci: Awaiting IRQ event" << std::endl;
		auto await = co_await helix_ng::awaitEvent(event, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();
		if(logIrqs)
			std::cout << "ehci: IRQ event fired (sequence: " << sequence << "), bits: "
					<< await.bitset() << std::endl;

		auto bits = arch::bit_value<uint32_t>(await.bitset());

		// TODO: The kernlet should write the status register!
		if(bits & usbsts::errorIrq)
			printf("\e[31mehci: Error interrupt\e[39m\n");
		_operational.store(op_regs::usbsts,
				usbsts::transactionIrq(bits & usbsts::transactionIrq)
				| usbsts::errorIrq(bits & usbsts::errorIrq)
				| usbsts::portChange(bits & usbsts::portChange));

		if((bits & usbsts::transactionIrq)
				|| (bits & usbsts::errorIrq)) {
			if(logIrqs)
				std::cout << "ehci: Processing transfers" << std::endl;
			_progressSchedule();
		}

		if(bits & usbsts::portChange) {
			if(logIrqs)
				std::cout << "ehci: Checking ports" << std::endl;
			_checkPorts();
		}
	}
}

// ------------------------------------------------------------------------
// Controller: Device management.
// ------------------------------------------------------------------------

async::result<frg::expected<proto::UsbError, std::string>>
Controller::deviceDescriptor(int address) {
	arch::dma_object<proto::SetupPacket> get_header{&schedulePool};
	get_header->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_header->request = proto::request_type::getDescriptor;
	get_header->value = proto::descriptor_type::device << 8;
	get_header->index = 0;
	get_header->length = 8;

	arch::dma_object<proto::DeviceDescriptor> descriptor{&schedulePool};
	FRG_CO_TRY(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			get_header, descriptor.view_buffer().subview(0, 8)}));

	// Read the rest of the device descriptor.
	arch::dma_object<proto::SetupPacket> get_descriptor{&schedulePool};
	get_descriptor->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_descriptor->request = proto::request_type::getDescriptor;
	get_descriptor->value = proto::descriptor_type::device << 8;
	get_descriptor->index = 0;
	get_descriptor->length = sizeof(proto::DeviceDescriptor);

	FRG_CO_TRY(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			get_descriptor, descriptor.view_buffer()}));
	assert(descriptor->length == sizeof(proto::DeviceDescriptor));

	std::string copy((char *)descriptor.data(), sizeof(proto::DeviceDescriptor));
	co_return std::move(copy);
}

async::result<frg::expected<proto::UsbError, std::string>>
Controller::configurationDescriptor(int address, uint8_t configuration) {
	// Read the descriptor header that contains the hierachy size.
	arch::dma_object<proto::SetupPacket> get_header{&schedulePool};
	get_header->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_header->request = proto::request_type::getDescriptor;
	get_header->value = proto::descriptor_type::configuration << 8 | configuration;
	get_header->index = 0;
	get_header->length = sizeof(proto::ConfigDescriptor);

	arch::dma_object<proto::ConfigDescriptor> header{&schedulePool};
	FRG_CO_TRY(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			get_header, header.view_buffer()}));
	assert(header->length == sizeof(proto::ConfigDescriptor));

	// Read the whole descriptor hierachy.
	arch::dma_object<proto::SetupPacket> get_descriptor{&schedulePool};
	get_descriptor->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toHost;
	get_descriptor->request = proto::request_type::getDescriptor;
	get_descriptor->value = proto::descriptor_type::configuration << 8 | configuration;
	get_descriptor->index = 0;
	get_descriptor->length = header->totalLength;

	arch::dma_buffer descriptor{&schedulePool, header->totalLength};
	FRG_CO_TRY(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToHost,
			get_descriptor, descriptor}));

	// TODO: This function should return a arch::dma_buffer!
	std::string copy((char *)descriptor.data(), header->totalLength);
	co_return std::move(copy);
}

async::result<frg::expected<proto::UsbError>>
Controller::useConfiguration(int address, int configuration) {
	arch::dma_object<proto::SetupPacket> set_config{&schedulePool};
	set_config->type = proto::setup_type::targetDevice | proto::setup_type::byStandard
			| proto::setup_type::toDevice;
	set_config->request = proto::request_type::setConfig;
	set_config->value = configuration;
	set_config->index = 0;
	set_config->length = 0;

	FRG_CO_TRY(co_await transfer(address, 0, proto::ControlTransfer{proto::kXferToDevice,
			set_config, arch::dma_buffer_view{}}));
	co_return {};
}

async::result<frg::expected<proto::UsbError>>
Controller::useInterface(int address, uint8_t configIndex, uint8_t configValue, int interface, int alternative) {
	(void) interface;
	assert(!alternative);

	std::optional<uint8_t> valueByIndex;

	auto descriptor = FRG_CO_TRY(co_await configurationDescriptor(address, configIndex));
	proto::walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		(void)length;

		if(type == proto::descriptor_type::configuration) {
			auto desc = (proto::ConfigDescriptor *)p;
			valueByIndex = desc->configValue;
		}

		if(type != proto::descriptor_type::endpoint)
			return;
		auto desc = (proto::EndpointDescriptor *)p;

		// TODO: Pay attention to interface/alternative.

		auto packet_size = desc->maxPacketSize & 0x7FF;

		// TODO: Set QH multiplier for high-bandwidth endpoints.
		if(desc->maxPacketSize & 0x1800)
			std::cout << "\e[35mehci: Endpoint is high bandwidth\e[39m" << std::endl;

		int pipe = info.endpointNumber.value();
		if(info.endpointIn.value()) {
			if(logDeviceEnumeration)
				std::cout << "ehci: Setting up IN pipe " << pipe
						<< " (max. packet size: " << desc->maxPacketSize << ")" << std::endl;
			_activeDevices[address].inStates[pipe].maxPacketSize = packet_size;
			_activeDevices[address].inStates[pipe].queueEntity
					= new QueueEntity{arch::dma_object<QueueHead>{&schedulePool},
							address, pipe, proto::PipeType::in, desc->maxPacketSize};
			this->_linkAsync(_activeDevices[address].inStates[pipe].queueEntity);
		}else{
			if(logDeviceEnumeration)
				std::cout << "ehci: Setting up OUT pipe " << pipe
						<< " (max. packet size: " << desc->maxPacketSize << ")" << std::endl;
			_activeDevices[address].outStates[pipe].maxPacketSize = packet_size;
			_activeDevices[address].outStates[pipe].queueEntity
					= new QueueEntity{arch::dma_object<QueueHead>{&schedulePool},
							address, pipe, proto::PipeType::out, desc->maxPacketSize};
			this->_linkAsync(_activeDevices[address].outStates[pipe].queueEntity);
		}
	});

	assert(valueByIndex);
	// Bail out if the user has no idea what they're asking for
	// A little late, but better late than never...
	if (*valueByIndex != configValue) {
		printf("ehci: useConfiguration(%u, %u) called, but that configuration has bConfigurationValue = %u???\n",
				configIndex, configValue, *valueByIndex);
		co_return proto::UsbError::other;
	}

	co_return frg::success;
}

// ------------------------------------------------------------------------
// Schedule classes.
// ------------------------------------------------------------------------

Controller::QueueEntity::QueueEntity(arch::dma_object<QueueHead> the_head,
		int address, int pipe, proto::PipeType type, size_t packet_size)
: head(std::move(the_head)) {
	head->horizontalPtr.store(qh_horizontal::terminate(false)
			| qh_horizontal::typeSelect(0x01)
			| qh_horizontal::horizontalPtr(schedulePointer(head.data())));
	head->flags.store(qh_flags::deviceAddr(address)
			| qh_flags::endpointNumber(pipe)
			| qh_flags::endpointSpeed(0x02)
			| qh_flags::manualDataToggle(type == proto::PipeType::control)
			| qh_flags::maxPacketLength(packet_size));
	head->mask.store(qh_mask::interruptScheduleMask(0x00)
			| qh_mask::multiplier(0x01));
	head->curTd.store(qh_curTd::curTd(0x00));
	head->nextTd.store(qh_nextTd::terminate(true));
	head->altTd.store(qh_altTd::terminate(true));
	head->status.store(qh_status::active(false));
	head->bufferPtr0.store(qh_buffer::bufferPtr(0));
	head->bufferPtr1.store(qh_buffer::bufferPtr(0));
	head->bufferPtr2.store(qh_buffer::bufferPtr(0));
	head->bufferPtr3.store(qh_buffer::bufferPtr(0));
	head->bufferPtr4.store(qh_buffer::bufferPtr(0));
}

bool Controller::QueueEntity::getReclaim() {
	return head->flags.load() & qh_flags::reclaimHead;
}

void Controller::QueueEntity::setReclaim(bool reclaim) {
	auto flags = head->flags.load();
	head->flags.store((flags & ~qh_flags::reclaimHead) | qh_flags::reclaimHead(reclaim));
}

void Controller::QueueEntity::setAddress(int address) {
	auto flags = head->flags.load();
	head->flags.store((flags & ~qh_flags::deviceAddr) | qh_flags::deviceAddr(address));
}

// ------------------------------------------------------------------------
// Transfer functions.
// ------------------------------------------------------------------------

async::result<frg::expected<proto::UsbError, size_t>>
Controller::transfer(int address, int pipe, proto::ControlTransfer info) {
	auto device = &_activeDevices[address];
	auto endpoint = &device->controlStates[pipe];

	auto transaction = _buildControl(info.flags,
			info.setup, info.buffer,  endpoint->maxPacketSize);
	auto future = transaction->promise.get_future();
	_linkTransaction(endpoint->queueEntity, transaction);
	co_return *(co_await future.get());
}

async::result<frg::expected<proto::UsbError, size_t>>
Controller::transfer(int address, proto::PipeType type, int pipe,
		proto::InterruptTransfer info) {
	// TODO: Ensure pipe type matches transfer direction.
	auto device = &_activeDevices[address];
	EndpointSlot *endpoint;
	if(type == proto::PipeType::in) {
		endpoint = &device->inStates[pipe];
	}else{
		assert(type == proto::PipeType::out);
		endpoint = &device->outStates[pipe];
	}

	auto transaction = _buildInterruptOrBulk(info.flags,
			info.buffer, endpoint->maxPacketSize, info.lazyNotification);
	auto future = transaction->promise.get_future();
	_linkTransaction(endpoint->queueEntity, transaction);
	co_return *(co_await future.get());
}

async::result<frg::expected<proto::UsbError, size_t>>
Controller::transfer(int address, proto::PipeType type, int pipe,
		proto::BulkTransfer info) {
	// TODO: Ensure pipe type matches transfer direction.
	auto device = &_activeDevices[address];
	EndpointSlot *endpoint;
	if(type == proto::PipeType::in) {
		endpoint = &device->inStates[pipe];
	}else{
		assert(type == proto::PipeType::out);
		endpoint = &device->outStates[pipe];
	}

	auto transaction = _buildInterruptOrBulk(info.flags,
			info.buffer, endpoint->maxPacketSize, info.lazyNotification);
	auto future = transaction->promise.get_future();
	_linkTransaction(endpoint->queueEntity, transaction);
	co_return *(co_await future.get());
}


auto Controller::_buildControl(proto::XferFlags dir,
		arch::dma_object_view<proto::SetupPacket> setup, arch::dma_buffer_view buffer,
		size_t) -> Transaction * {
	assert((dir == proto::kXferToDevice) || (dir == proto::kXferToHost));

	size_t num_data = (buffer.size() + 0x3FFF) / 0x4000;
	assert(num_data <= 1);
	arch::dma_array<TransferDescriptor> transfers{&schedulePool, num_data + 2};

	// TODO: This code is horribly broken if the setup packet or
	// one of the data packets crosses a page boundary.

	transfers[0].nextTd.store(td_ptr::ptr(schedulePointer(&transfers[1]))
			| td_ptr::terminate(false));
	transfers[0].altTd.store(td_ptr::terminate(true));
	transfers[0].status.store(td_status::active(true)
			| td_status::pidCode(2)
			| td_status::interruptOnComplete(true)
			| td_status::totalBytes(sizeof(proto::SetupPacket)));
	transfers[0].bufferPtr0.store(td_buffer::bufferPtr(physicalPointer(setup.data())));
	transfers[0].extendedPtr0.store(0);

	size_t progress = 0;
	for(size_t i = 0; i < num_data; i++) {
		size_t chunk = std::min(size_t(0x4000), buffer.size() - progress);
		assert(chunk);
		transfers[i + 1].nextTd.store(td_ptr::ptr(schedulePointer(&transfers[i + 2])));
		transfers[i + 1].altTd.store(td_ptr::terminate(true));
		// TODO: If there is more than one TD we need to compute the correct data toggle.
		transfers[i + 1].status.store(td_status::active(true)
				| td_status::pidCode(dir == proto::kXferToDevice ? 0 : 1)
				| td_status::interruptOnComplete(true)
				| td_status::totalBytes(chunk)
				| td_status::dataToggle(true));
		// FIXME: Support larger buffers!
		transfers[i + 1].bufferPtr0.store(td_buffer::bufferPtr(
				physicalPointer((char *)buffer.data() + progress)));
		transfers[i + 1].extendedPtr0.store(0);
		progress += chunk;
	}

	// The status stage always sends a DATA1 token.
	transfers[num_data + 1].nextTd.store(td_ptr::terminate(true));
	transfers[num_data + 1].altTd.store(td_ptr::terminate(true));
	transfers[num_data + 1].status.store(td_status::active(true)
			| td_status::pidCode(dir == proto::kXferToDevice ? 1 : 0)
			| td_status::interruptOnComplete(true)
			| td_status::dataToggle(true));

	return new Transaction{std::move(transfers), buffer.size()};
}

auto Controller::_buildInterruptOrBulk(proto::XferFlags dir,
		arch::dma_buffer_view buffer, size_t max_packet_size,
		bool lazy_notification) -> Transaction * {
	assert((dir == proto::kXferToDevice) || (dir == proto::kXferToHost));

	// Maximum size that can be transferred in a single qTD starting from a certain offset.
	// Note that we need to make sure that we do not generate short packets.
	auto td_size = [&] (size_t offset) {
		auto misalign = ((uintptr_t)buffer.data() + offset) & 0xFFF;
		auto available = 0x5000 - misalign;
		return available - available % max_packet_size;
	};

	// Compute the number of required qTDs.
	size_t num_data = 1;
	auto projected = td_size(0);
	while(projected < buffer.size()) {
		projected += td_size(projected);
		num_data++;
	}

	if(logPackets)
		std::cout << "ehci: Building transfer using " << num_data << " TDs" << std::endl;

	// Finally construct each qTD.
	arch::dma_array<TransferDescriptor> transfers{&schedulePool, num_data};

	size_t progress = 0;
	for(size_t i = 0; i < num_data; i++) {
		size_t chunk = std::min(td_size(progress), buffer.size() - progress);
		assert(chunk);
		if(i + 1 < num_data) {
			transfers[i].nextTd.store(td_ptr::ptr(schedulePointer(&transfers[i + 1])));
		}else{
			transfers[i].nextTd.store(td_ptr::terminate(true));
		}
		transfers[i].altTd.store(td_ptr::terminate(true));
		transfers[i].status.store(td_status::active(true)
				| td_status::pidCode(dir == proto::kXferToDevice ? 0x00 : 0x01)
				| td_status::interruptOnComplete(i + 1 == num_data && !lazy_notification)
				| td_status::totalBytes(chunk));

		transfers[i].bufferPtr0.store(td_buffer::bufferPtr(physicalPointer((char *)buffer.data()
				+ progress)));
		transfers[i].extendedPtr0.store(0);

		auto misalign = ((uintptr_t)buffer.data() + progress) & 0xFFF;
		if(progress + 0x1000 - misalign < buffer.size()) {
			transfers[i].bufferPtr1.store(td_buffer::bufferPtr(physicalPointer((char *)buffer.data()
					+ progress + 0x1000 - misalign)));
			transfers[i].extendedPtr1.store(0);
		}
		if(progress + 0x2000 - misalign < buffer.size()) {
			transfers[i].bufferPtr2.store(td_buffer::bufferPtr(physicalPointer((char *)buffer.data()
					+ progress + 0x2000 - misalign)));
			transfers[i].extendedPtr2.store(0);
		}
		if(progress + 0x3000 - misalign < buffer.size()) {
			transfers[i].bufferPtr3.store(td_buffer::bufferPtr(physicalPointer((char *)buffer.data()
					+ progress + 0x3000 - misalign)));
			transfers[i].extendedPtr3.store(0);
		}
		if(progress + 0x4000 - misalign < buffer.size()) {
			transfers[i].bufferPtr4.store(td_buffer::bufferPtr(physicalPointer((char *)buffer.data()
					+ progress + 0x4000 - misalign)));
			transfers[i].extendedPtr4.store(0);
		}
		progress += chunk;
	}
	assert(progress == buffer.size());

	return new Transaction{std::move(transfers), buffer.size()};
}


async::result<frg::expected<proto::UsbError, size_t>> Controller::_directTransfer(proto::ControlTransfer info,
		QueueEntity *queue, size_t max_packet_size) {
	auto transaction = _buildControl(info.flags,
			info.setup, info.buffer, max_packet_size);
	auto future = transaction->promise.get_future();
	_linkTransaction(queue, transaction);
	co_return *(co_await future.get());
}

// ------------------------------------------------------------------------
// Schedule management.
// ------------------------------------------------------------------------

void Controller::_linkAsync(QueueEntity *entity) {
	entity->setReclaim(true);
	if(_asyncSchedule.empty()) {
		entity->head->horizontalPtr.store(
				qh_horizontal::horizontalPtr(schedulePointer(entity->head.data()))
				| qh_horizontal::typeSelect(1));
		_operational.store(op_regs::asynclistaddr, schedulePointer(entity->head.data()));
		_operational.store(op_regs::usbcmd, usbcmd::asyncEnable(true)
				| usbcmd::run(true) | usbcmd::irqThreshold(0x08));
	}else{
		entity->head->horizontalPtr.store(
				qh_horizontal::horizontalPtr(schedulePointer(_asyncSchedule.front().head.data()))
				| qh_horizontal::typeSelect(1));
		_asyncSchedule.back().head->horizontalPtr.store(
				qh_horizontal::horizontalPtr(schedulePointer(entity->head.data()))
				| qh_horizontal::typeSelect(1));
		assert(_asyncSchedule.back().getReclaim());
		_asyncSchedule.back().setReclaim(false);
	}
	_asyncSchedule.push_back(*entity);
}

void Controller::_linkTransaction(QueueEntity *queue, Transaction *transaction) {
	assert(transaction->transfers.size());

	if(queue->transactions.empty()) {
		if(logSubmits)
			std::cout << "ehci: Linking in _linkTransaction" << std::endl;
		auto status = queue->head->status.load();
		assert(queue->head->nextTd.load() & td_ptr::terminate);
		assert(!(status & qh_status::active));
		assert(!(status & qh_status::halted));
		assert(!(status & qh_status::totalBytes));
		auto current = (queue->head->curTd.load() & qh_curTd::curTd);
		auto pointer = schedulePointer(&transaction->transfers[0]);
		queue->head->nextTd.store(qh_nextTd::nextTd(pointer));

		if(debugLinking) {
			std::cout << "ehci: Waiting for AdvanceQueue" << std::endl;
			uint32_t update;
			do {
				update = (queue->head->curTd.load() & qh_curTd::curTd);
				usleep(1'000);
			} while(current == update);

			// TODO: We could ensure that the new TD pointer is part of the transaction.
			std::cout << "ehci: AdvanceQueue to new transaction" << std::endl;
		}
	}

	queue->transactions.push_back(*transaction);
}

void Controller::_progressSchedule() {
	auto it = _asyncSchedule.begin();
	while(it != _asyncSchedule.end()) {
		_progressQueue(&(*it));
		++it;
	}
}

void Controller::_progressQueue(QueueEntity *entity) {
	if(entity->transactions.empty())
		return;

	auto active = &entity->transactions.front();
	while(active->numComplete < active->transfers.size()) {
		auto transfer = &active->transfers[active->numComplete];
		if((transfer->status.load() & td_status::active)
				|| (transfer->status.load() & td_status::halted)
				|| (transfer->status.load() & td_status::transactionError)
				|| (transfer->status.load() & td_status::babbleDetected)
				|| (transfer->status.load() & td_status::dataBufferError))
			break;

		auto lost = (transfer->status.load() & td_status::totalBytes);
		assert(!lost); // TODO: Support short packets.

		active->numComplete++;
		active->lostSize += lost;
	}

	auto current = active->numComplete;
	if(current == active->transfers.size()) {
		if(logSubmits)
			std::cout << "ehci: Transfer complete!" << std::endl;
		assert(active->fullSize >= active->lostSize);
		active->promise.set_value(active->fullSize - active->lostSize);

		// Clean up the Queue.
		entity->transactions.pop_front();
		//delete active;
		// TODO: _reclaim(active);

		// Schedule the next transaction.
		assert(entity->head->nextTd.load() & td_ptr::terminate);
		if(!entity->transactions.empty()) {
			if(logSubmits)
				std::cout << "ehci: Linking in _progressQueue" << std::endl;
			auto front = &entity->transactions.front();
			entity->head->nextTd.store(qh_nextTd::nextTd(schedulePointer(&front->transfers[0])));
		}
	}else if((active->transfers[current].status.load() & td_status::halted)
			|| (active->transfers[current].status.load() & td_status::transactionError)
			|| (active->transfers[current].status.load() & td_status::babbleDetected)
			|| (active->transfers[current].status.load() & td_status::dataBufferError)) {
		printf("Transfer error!\n");

		_dump(entity);

		// Clean up the Queue.
		entity->transactions.pop_front();
		//delete active;
		// TODO: _reclaim(active);
	}
}

// ----------------------------------------------------------------------------
// Port management.
// ----------------------------------------------------------------------------

// TODO: This should be async.
async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>>
Controller::resetPort(int number) {
	auto offset = _space.load(cap_regs::caplength);
	auto port_space = _space.subspace(offset + 0x44 + (4 * number));

//	std::cout << "ehci: Port reset." << std::endl;
	port_space.store(port_regs::sc, portsc::portReset(true));

	uint64_t tick;
	HEL_CHECK(helGetClock(&tick));

	helix::AwaitClock await_clock;
	auto &&submit = helix::submitAwaitClock(&await_clock, tick + 50'000'000,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(await_clock.error());

	port_space.store(port_regs::sc, portsc::portReset(false));

//	std::cout << "ehci: Waiting for reset to complete." << std::endl;
	arch::bit_value<uint32_t> sc{0};
	do {
		sc = port_space.load(port_regs::sc);
	} while(sc & portsc::portReset);

	auto &port = _rootHub->port(number);

	if(sc & portsc::enableStatus) {
		assert(!(sc & portsc::enableChange)); // See handleIrqs().
		std::cout << "ehci: Port " << number << " was enabled." << std::endl;

		port.state.changes |= proto::HubStatus::enable;
		port.state.status |= proto::HubStatus::enable;
		port.pollEv.raise();

		co_return proto::DeviceSpeed::highSpeed;
	}else{
		std::cout << "ehci: Device on port " << number << " is not high-speed" << std::endl;
		port_space.store(port_regs::sc, portsc::portOwner(true));

		co_return proto::UsbError::unsupported;
	}
}

// ----------------------------------------------------------------------------
// Debugging functions.
// ----------------------------------------------------------------------------

void Controller::_dump(QueueEntity *entity) {
	std::cout << "queue_head_status: " << std::endl;
	std::cout << "    pingError: " << (int)(entity->head->status.load()
			& qh_status::pingError) << std::endl;
	std::cout << "    splitXState: " << (int)(entity->head->status.load()
			& qh_status::splitXState) << std::endl;
	std::cout << "    missedFrame: " << (int)(entity->head->status.load()
			& qh_status::missedFrame) << std::endl;
	std::cout << "    transactionError: " << (int)(entity->head->status.load()
			& qh_status::transactionError) << std::endl;
	std::cout << "    babbleDetected: " << (int)(entity->head->status.load()
			& qh_status::babbleDetected) << std::endl;
	std::cout << "    dataBufferError: " << (int)(entity->head->status.load()
			& qh_status::dataBufferError) << std::endl;
	std::cout << "    halted: " << (int)(entity->head->status.load()
			& qh_status::halted) << std::endl;
	std::cout << "    pidCode: " << (int)(entity->head->status.load()
			& qh_status::pidCode) << std::endl;
	std::cout << "    errorCounter: " << (int)(entity->head->status.load()
			& qh_status::errorCounter) << std::endl;
	std::cout << "    cPage: " << (int)(entity->head->status.load()
			& qh_status::cPage) << std::endl;
	std::cout << "    interruptOnComplete: " << (int)(entity->head->status.load()
			& qh_status::interruptOnComplete) << std::endl;
	std::cout << "    totalBytes: " << (int)(entity->head->status.load()
			& qh_status::totalBytes) << std::endl;
	std::cout << "    dataToggle: " << (int)(entity->head->status.load()
			& qh_status::dataToggle) << std::endl;

	auto active = &entity->transactions.front();
	for(size_t i = 0; i < active->transfers.size(); i++) {
		auto &transfer = active->transfers[i];
		std::cout << "transfer " << i << ": " << std::endl;
		std::cout << "    pingError: " << (int)(transfer.status.load()
				& td_status::pingError) << std::endl;
		std::cout << "    splitXState: " << (int)(transfer.status.load()
				& td_status::splitXState) << std::endl;
		std::cout << "    missedFrame: " << (int)(transfer.status.load()
				& td_status::missedFrame) << std::endl;
		std::cout << "    transactionError: " << (int)(transfer.status.load()
				& td_status::transactionError) << std::endl;
		std::cout << "    babbleDetected: " << (int)(transfer.status.load()
				& td_status::babbleDetected) << std::endl;
		std::cout << "    dataBufferError: " << (int)(transfer.status.load()
				& td_status::dataBufferError) << std::endl;
		std::cout << "    halted: " << (int)(transfer.status.load()
				& td_status::halted) << std::endl;
		std::cout << "    pidCode: " << (int)(transfer.status.load()
				& td_status::pidCode) << std::endl;
		std::cout << "    errorCounter: " << (int)(transfer.status.load()
				& td_status::errorCounter) << std::endl;
		std::cout << "    cPage: " << (int)(transfer.status.load()
				& td_status::cPage) << std::endl;
		std::cout << "    interruptOnComplete: " << (int)(transfer.status.load()
				& td_status::interruptOnComplete) << std::endl;
		std::cout << "    totalBytes: " << (int)(transfer.status.load()
				& td_status::totalBytes) << std::endl;
		std::cout << "    dataToggle: " << (int)(transfer.status.load()
				& td_status::dataToggle) << std::endl;
	}
}

// ----------------------------------------------------------------
// Root hub.
// ----------------------------------------------------------------

Controller::RootHub::RootHub(Controller *controller)
: Hub{nullptr, 0}, _controller{controller} {
	for (int i = 0; i < _controller->_numPorts; i++) {
		_ports.push_back(std::make_unique<Port>());
	}
}

size_t Controller::RootHub::numPorts() {
	return _ports.size();
}

async::result<proto::PortState> Controller::RootHub::pollState(int port) {
	co_return co_await _ports[port]->pollState();
}

async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>>
Controller::RootHub::issueReset(int port) {
	co_return co_await _controller->resetPort(port);
}

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

async::detached bindController(mbus_ng::Entity entity) {
	protocols::hw::Device device((co_await entity.getRemoteLane()).unwrap());
	auto info = co_await device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = co_await device.accessBar(0);
	auto irq = co_await device.accessIrq();

	helix::Mapping mapping{bar, info.barInfo[0].offset, info.barInfo[0].length};

	mbus_ng::Properties descriptor{
		{"generic.devtype", mbus_ng::StringItem{"usb-controller"}},
		{"generic.devsubtype", mbus_ng::StringItem{"ehci"}},
		{"usb.version.major", mbus_ng::StringItem{"2"}},
		{"usb.version.minor", mbus_ng::StringItem{"0"}},
		{"usb.root.parent", mbus_ng::StringItem{std::to_string(entity.id())}},
	};

	auto ehciEntity = (co_await mbus_ng::Instance::global().createEntity(
		"ehci-controller", descriptor)).unwrap();

	auto controller = std::make_shared<Controller>(std::move(device), std::move(ehciEntity), std::move(mapping),
			std::move(bar), std::move(irq));
	controller->initialize();
	globalControllers.push_back(std::move(controller));
}

async::detached observeControllers() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"pci-class", "0c"},
		mbus_ng::EqualsFilter{"pci-subclass", "03"},
		mbus_ng::EqualsFilter{"pci-interface", "20"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::cout << "ehci: Detected controller" << std::endl;
			bindController(std::move(entity));
		}
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "ehci: Starting driver";

//	HEL_CHECK(helSetPriority(kHelThisThread, 2));

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}

