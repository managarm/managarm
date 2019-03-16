
#include <assert.h>
#include <stdio.h>
#include <deque>
#include <experimental/optional>
#include <functional>
#include <iostream>
#include <memory>

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <fafnir/dsl.hpp>
#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/memory.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
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

std::vector<std::shared_ptr<Controller>> globalControllers;

// ----------------------------------------------------------------------------
// Enumerator.
// ----------------------------------------------------------------------------

Enumerator::Enumerator(Controller *controller)
: _controller{controller} { }

void Enumerator::connectPort(int port) {
	assert(_state == State::null);
	_state = State::resetting;
	_activePort = port;
	_reset();
}

void Enumerator::enablePort(int port) {
	assert(_state == State::resetting);
	assert(_activePort == port);
	_state = State::probing;
	_probe();
}

COFIBER_ROUTINE(cofiber::no_future, Enumerator::_reset(), ([=] {
	COFIBER_AWAIT _addressMutex.async_lock();
	_controller->resetPort(_activePort);
}))

COFIBER_ROUTINE(cofiber::no_future, Enumerator::_probe(), ([=] {
	COFIBER_AWAIT _controller->probeDevice();
	_addressMutex.unlock();
}))

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

async::result<std::string> DeviceState::configurationDescriptor() {
	return _controller->configurationDescriptor(_device);
}

COFIBER_ROUTINE(async::result<Configuration>, 
		DeviceState::useConfiguration(int number), ([=] {
	COFIBER_AWAIT _controller->useConfiguration(_device, number);
	COFIBER_RETURN(Configuration{std::make_shared<ConfigurationState>(_controller,
			_device, number)});
}))

async::result<void> DeviceState::transfer(ControlTransfer info) {
	return _controller->transfer(_device, 0, info);
}

// ----------------------------------------------------------------------------
// ConfigurationState
// ----------------------------------------------------------------------------

ConfigurationState::ConfigurationState(std::shared_ptr<Controller> controller,
		int device, int configuration)
: _controller{std::move(controller)}, _device(device), _configuration(configuration) { }

COFIBER_ROUTINE(async::result<Interface>, ConfigurationState::useInterface(int number,
		int alternative), ([=] {
	COFIBER_AWAIT _controller->useInterface(_device, number, alternative);
	COFIBER_RETURN(Interface{std::make_shared<InterfaceState>(_controller, _device, number)});
}))

// ----------------------------------------------------------------------------
// InterfaceState
// ----------------------------------------------------------------------------

InterfaceState::InterfaceState(std::shared_ptr<Controller> controller,
		int device, int interface)
: _controller{std::move(controller)}, _device(device), _interface(interface) { }

COFIBER_ROUTINE(async::result<Endpoint>, InterfaceState::getEndpoint(PipeType type,
		int number), ([=] {
	COFIBER_RETURN(Endpoint{std::make_shared<EndpointState>(_controller,
			_device, type, number)});
}))

// ----------------------------------------------------------------------------
// EndpointState
// ----------------------------------------------------------------------------

EndpointState::EndpointState(std::shared_ptr<Controller> controller,
		int device, PipeType type, int endpoint)
: _controller{std::move(controller)}, _device(device), _type(type), _endpoint(endpoint) { }

async::result<void> EndpointState::transfer(ControlTransfer info) {
	(void)info;
	assert(!"FIXME: Implement this");
	__builtin_unreachable();
}

async::result<size_t> EndpointState::transfer(InterruptTransfer info) {
	return _controller->transfer(_device, _type, _endpoint, info);
}

async::result<size_t> EndpointState::transfer(BulkTransfer info) {
	return _controller->transfer(_device, _type, _endpoint, info);
}

// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

Controller::Controller(protocols::hw::Device hw_device, helix::Mapping mapping,
		helix::UniqueDescriptor mmio, helix::UniqueIrq irq)
: _hwDevice{std::move(hw_device)}, _mapping{std::move(mapping)},
		_mmio{std::move(mmio)}, _irq{std::move(irq)},
		_space{_mapping.get()},
		_enumerator{this} { 
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

COFIBER_ROUTINE(cofiber::no_future, Controller::initialize(), ([=] {
	auto ext_pointer = _space.load(cap_regs::hccparams) & hccparams::extPointer;
	if(ext_pointer) {
		auto header = COFIBER_AWAIT _hwDevice.loadPciSpace(ext_pointer, 2);
		if(logControllerEnumeration)
			std::cout << "ehci: Extended capability: " << (header & 0xFF) << std::endl;

		assert((header & 0xFF) == 1);
		if(logControllerEnumeration && (COFIBER_AWAIT _hwDevice.loadPciSpace(ext_pointer + 2, 1)))
			std::cout << "ehci: Controller is owned by the BIOS" << std::endl;

		// TODO: We need a timeout here.
		assert(!(COFIBER_AWAIT _hwDevice.loadPciSpace(ext_pointer + 3, 1)));
		COFIBER_AWAIT _hwDevice.storePciSpace(ext_pointer + 3, 1, 1);
		while(COFIBER_AWAIT _hwDevice.loadPciSpace(ext_pointer + 2, 1)) {
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

	_checkPorts();
	handleIrqs();
}))

void Controller::_checkPorts() {
	assert(!(_space.load(cap_regs::hcsparams) & hcsparams::portPower));

	for(int i = 0; i < _numPorts; i++) {
		auto offset = _space.load(cap_regs::caplength);
		auto port_space = _space.subspace(offset + 0x44 + (4 * i));
		auto sc = port_space.load(port_regs::sc);
//		std::cout << "port " << i << " sc: " << static_cast<unsigned int>(sc) << std::endl;

		if(sc & portsc::enableChange) {
			// EHCI specifies that enableChange is only set on port error.
			port_space.store(port_regs::sc, portsc::enableChange(true)
					| portsc::portOwner(sc & portsc::portOwner));
			if(!(sc & portsc::enableStatus)) {
				std::cout << "ehci: Port " << i << " disabled due to error" << std::endl;
			}else{
				std::cout << "ehci: Spurious portsc::enableChange" << std::endl;
			}
		}

		if(sc & portsc::connectChange) {
			// TODO: Be careful to set the correct bits (e.g. suspend once we support it).
			port_space.store(port_regs::sc, portsc::connectChange(true)
					| portsc::portOwner(sc & portsc::portOwner));
			if(sc & portsc::connectStatus) {
				if((sc & portsc::lineStatus) == 1) {
					if(logDeviceEnumeration)
						std::cout << "ehci: Device on port " << i << " is low-speed" << std::endl;
					port_space.store(port_regs::sc, portsc::portOwner(true));
				}else{
					if(logDeviceEnumeration)
						std::cout << "ehci: Connect on port " << i << std::endl;
					_enumerator.connectPort(i);
				}
			}else{
				if(logDeviceEnumeration)
					std::cout << "ehci: Disconnect on port " << i << std::endl;
			}
		}
	}
}

COFIBER_ROUTINE(async::result<void>, Controller::probeDevice(), ([=] {
	// This queue will become the default control pipe of our new device.
	auto dma_obj = arch::dma_object<QueueHead>{&schedulePool};
	auto queue = new QueueEntity{std::move(dma_obj), 0, 0, PipeType::control, 64};
	_linkAsync(queue);

	// Allocate an address for the device.
	assert(!_addressStack.empty());
	auto address = _addressStack.front();
	_addressStack.pop();

	if(logDeviceEnumeration)
		std::cout << "ehci: Setting device address" << std::endl;

	arch::dma_object<SetupPacket> set_address{&schedulePool};
	set_address->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toDevice;
	set_address->request = request_type::setAddress;
	set_address->value = address;
	set_address->index = 0;
	set_address->length = 0;

	COFIBER_AWAIT _directTransfer(ControlTransfer{kXferToDevice,
			set_address, arch::dma_buffer_view{}}, queue, 0);

	queue->setAddress(address);

	// Enquire the maximum packet size of the default control pipe.
	if(logDeviceEnumeration)
		std::cout << "ehci: Getting device descriptor header" << std::endl;

	arch::dma_object<SetupPacket> get_header{&schedulePool};
	get_header->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toHost;
	get_header->request = request_type::getDescriptor;
	get_header->value = descriptor_type::device << 8;
	get_header->index = 0;
	get_header->length = 8;

	arch::dma_object<DeviceDescriptor> descriptor{&schedulePool};
	COFIBER_AWAIT _directTransfer(ControlTransfer{kXferToHost,
			get_header, descriptor.view_buffer().subview(0, 8)}, queue, 8);

	_activeDevices[address].controlStates[0].queueEntity = queue;
	_activeDevices[address].controlStates[0].maxPacketSize = descriptor->maxPacketSize;

	// Read the rest of the device descriptor.
	if(logDeviceEnumeration)
		std::cout << "ehci: Getting full device descriptor" << std::endl;

	arch::dma_object<SetupPacket> get_descriptor{&schedulePool};
	get_descriptor->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toHost;
	get_descriptor->request = request_type::getDescriptor;
	get_descriptor->value = descriptor_type::device << 8;
	get_descriptor->index = 0;
	get_descriptor->length = sizeof(DeviceDescriptor);

	COFIBER_AWAIT transfer(address, 0, ControlTransfer{kXferToHost,
			get_descriptor, descriptor.view_buffer()});
	assert(descriptor->length == sizeof(DeviceDescriptor));

	// TODO: Read configuration descriptor from the device.

	char class_code[3], sub_class[3], protocol[3];
	char vendor[5], product[5], release[5];
	sprintf(class_code, "%.2x", descriptor->deviceClass);
	sprintf(sub_class, "%.2x", descriptor->deviceSubclass);
	sprintf(protocol, "%.2x", descriptor->deviceProtocol);
	sprintf(vendor, "%.4x", descriptor->idVendor);
	sprintf(product, "%.4x", descriptor->idProduct);
	sprintf(release, "%.4x", descriptor->bcdDevice);

	mbus::Properties mbus_desc{
		{"usb.type", mbus::StringItem{"device"}},
		{"usb.vendor", mbus::StringItem{vendor}},
		{"usb.product", mbus::StringItem{product}},
		{"usb.class", mbus::StringItem{class_code}},
		{"usb.subclass", mbus::StringItem{sub_class}},
		{"usb.protocol", mbus::StringItem{protocol}},
		{"usb.release", mbus::StringItem{release}}
	};

	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	char name[3];
	sprintf(name, "%.2x", address);

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		auto state = std::make_shared<DeviceState>(shared_from_this(), address);
		protocols::usb::serve(Device{std::move(state)}, std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});

	COFIBER_AWAIT root.createObject(name, mbus_desc, std::move(handler));
	COFIBER_RETURN();
}))

COFIBER_ROUTINE(cofiber::no_future, Controller::handleIrqs(), ([=] {
	COFIBER_AWAIT connectKernletCompiler();

	std::vector<uint8_t> kernlet_program;
	fnr::emit_to(std::back_inserter(kernlet_program),
		// Load the USBSTS register.
		fnr::s_define{} (
			fnr::intrin{"__mmio_read32", 2, 1} (
				fnr::binding{0}, // EHCI MMIO region (bound to slot 0).
				fnr::binding{1} // EHCI MMIO offset (bound to slot 1).
					 + fnr::literal{4} // Offset of USBSTS.
			) & fnr::literal{23} // USB transaction, error, port change and host error bits.
		),
		// Ack the IRQ iff one of the bits was set.
		fnr::check_if{},
			fnr::s_value{0},
		fnr::then{},
			// Write back the interrupt bits to USBSTS to deassert the IRQ.
			fnr::intrin{"__mmio_write32", 3, 0} (
				fnr::binding{0}, // EHCI MMIO region (bound to slot 0).
				fnr::binding{1} // EHCI MMIO offset (bound to slot 1).
					+ fnr::literal{4}, // Offset of USBSTS.
				fnr::s_value{0}
			),
			// Trigger the bitset event (bound to slot 2).
			fnr::intrin{"__trigger_bitset", 2, 0} (
				fnr::binding{2},
				fnr::s_value{0}
			),
			fnr::s_define{} ( fnr::literal{1} ),
		fnr::else_then{},
			fnr::s_define{} ( fnr::literal{2} ),
		fnr::end{}
	);

	auto kernlet_object = COFIBER_AWAIT compile(kernlet_program.data(),
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

	COFIBER_AWAIT _hwDevice.enableBusIrq();

	// TODO: We should not need this kick anymore.
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckKick, 0));

	uint64_t sequence = 0;
	while(true) {
		if(logIrqs)
			std::cout << "ehci: Awaiting IRQ event" << std::endl;
		helix::AwaitEvent await_event;
		auto &&submit = helix::submitAwaitEvent(event, &await_event,
				sequence, helix::Dispatcher::global());
		COFIBER_AWAIT submit.async_wait();
		HEL_CHECK(await_event.error());
		sequence = await_event.sequence();
		if(logIrqs)
			std::cout << "ehci: IRQ event fired (sequence: " << sequence << "), bits: "
					<< await_event.bitset() << std::endl;

		auto bits = arch::bit_value<uint32_t>(await_event.bitset());

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
}))

// ------------------------------------------------------------------------
// Controller: Device management.
// ------------------------------------------------------------------------

COFIBER_ROUTINE(async::result<std::string>, Controller::configurationDescriptor(int address),
		([=] {
	// Read the descriptor header that contains the hierachy size.
	arch::dma_object<SetupPacket> get_header{&schedulePool};
	get_header->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toHost;
	get_header->request = request_type::getDescriptor;
	get_header->value = descriptor_type::configuration << 8;
	get_header->index = 0;
	get_header->length = sizeof(ConfigDescriptor);

	arch::dma_object<ConfigDescriptor> header{&schedulePool};
	COFIBER_AWAIT transfer(address, 0, ControlTransfer{kXferToHost,
			get_header, header.view_buffer()});
	assert(header->length == sizeof(ConfigDescriptor));

	// Read the whole descriptor hierachy.
	arch::dma_object<SetupPacket> get_descriptor{&schedulePool};
	get_descriptor->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toHost;
	get_descriptor->request = request_type::getDescriptor;
	get_descriptor->value = descriptor_type::configuration << 8;
	get_descriptor->index = 0;
	get_descriptor->length = header->totalLength;

	arch::dma_buffer descriptor{&schedulePool, header->totalLength};
	COFIBER_AWAIT transfer(address, 0, ControlTransfer{kXferToHost,
			get_descriptor, descriptor});

	// TODO: This function should return a arch::dma_buffer!
	std::string copy((char *)descriptor.data(), header->totalLength);
	COFIBER_RETURN(std::move(copy));
}))

COFIBER_ROUTINE(async::result<void>, Controller::useConfiguration(int address,
		int configuration), ([=] {
	arch::dma_object<SetupPacket> set_config{&schedulePool};
	set_config->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toDevice;
	set_config->request = request_type::setConfig;
	set_config->value = configuration;
	set_config->index = 0;
	set_config->length = 0;

	COFIBER_AWAIT transfer(address, 0, ControlTransfer{kXferToDevice,
			set_config, arch::dma_buffer_view{}});

	COFIBER_RETURN();
}))

COFIBER_ROUTINE(async::result<void>, Controller::useInterface(int address,
		int interface, int alternative), ([=] {
	auto descriptor = COFIBER_AWAIT configurationDescriptor(address);
	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		(void)length;

		if(type != descriptor_type::endpoint)
			return;
		auto desc = (EndpointDescriptor *)p;

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
							address, pipe, PipeType::in, desc->maxPacketSize};
			this->_linkAsync(_activeDevices[address].inStates[pipe].queueEntity);
		}else{
			if(logDeviceEnumeration)
				std::cout << "ehci: Setting up OUT pipe " << pipe
						<< " (max. packet size: " << desc->maxPacketSize << ")" << std::endl;
			_activeDevices[address].outStates[pipe].maxPacketSize = packet_size;
			_activeDevices[address].outStates[pipe].queueEntity
					= new QueueEntity{arch::dma_object<QueueHead>{&schedulePool},
							address, pipe, PipeType::out, desc->maxPacketSize};
			this->_linkAsync(_activeDevices[address].outStates[pipe].queueEntity);
		}
	});

	COFIBER_RETURN();
}))

// ------------------------------------------------------------------------
// Schedule classes.
// ------------------------------------------------------------------------

Controller::QueueEntity::QueueEntity(arch::dma_object<QueueHead> the_head,
		int address, int pipe, PipeType type, size_t packet_size)
: head(std::move(the_head)) {
	head->horizontalPtr.store(qh_horizontal::terminate(false)
			| qh_horizontal::typeSelect(0x01) 
			| qh_horizontal::horizontalPtr(schedulePointer(head.data())));
	head->flags.store(qh_flags::deviceAddr(address)
			| qh_flags::endpointNumber(pipe)
			| qh_flags::endpointSpeed(0x02)
			| qh_flags::manualDataToggle(type == PipeType::control)
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

async::result<void> Controller::transfer(int address, int pipe, ControlTransfer info) {
	auto device = &_activeDevices[address];
	auto endpoint = &device->controlStates[pipe];

	auto transaction = _buildControl(info.flags,
			info.setup, info.buffer,  endpoint->maxPacketSize);
	_linkTransaction(endpoint->queueEntity, transaction);
	return transaction->voidPromise.async_get();
}

async::result<size_t> Controller::transfer(int address, PipeType type, int pipe,
		InterruptTransfer info) {
	// TODO: Ensure pipe type matches transfer direction.
	auto device = &_activeDevices[address];
	EndpointSlot *endpoint;
	if(type == PipeType::in) {
		endpoint = &device->inStates[pipe];
	}else{
		assert(type == PipeType::out);
		endpoint = &device->outStates[pipe];
	}

	auto transaction = _buildInterruptOrBulk(info.flags,
			info.buffer, endpoint->maxPacketSize, info.lazyNotification);
	_linkTransaction(endpoint->queueEntity, transaction);
	return transaction->promise.async_get();
}

async::result<size_t> Controller::transfer(int address, PipeType type, int pipe,
		BulkTransfer info) {
	// TODO: Ensure pipe type matches transfer direction.
	auto device = &_activeDevices[address];
	EndpointSlot *endpoint;
	if(type == PipeType::in) {
		endpoint = &device->inStates[pipe];
	}else{
		assert(type == PipeType::out);
		endpoint = &device->outStates[pipe];
	}

	auto transaction = _buildInterruptOrBulk(info.flags,
			info.buffer, endpoint->maxPacketSize, info.lazyNotification);
	_linkTransaction(endpoint->queueEntity, transaction);
	return transaction->promise.async_get();
}


auto Controller::_buildControl(XferFlags dir,
		arch::dma_object_view<SetupPacket> setup, arch::dma_buffer_view buffer,
		size_t max_packet_size) -> Transaction * {
	assert((dir == kXferToDevice) || (dir == kXferToHost));

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
			| td_status::totalBytes(sizeof(SetupPacket)));
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
				| td_status::pidCode(dir == kXferToDevice ? 0 : 1)
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
			| td_status::pidCode(dir == kXferToDevice ? 1 : 0)
			| td_status::interruptOnComplete(true)
			| td_status::dataToggle(true));

	return new Transaction{std::move(transfers), buffer.size()};
}

auto Controller::_buildInterruptOrBulk(XferFlags dir,
		arch::dma_buffer_view buffer, size_t max_packet_size,
		bool lazy_notification) -> Transaction * {
	assert((dir == kXferToDevice) || (dir == kXferToHost));

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
				| td_status::pidCode(dir == kXferToDevice ? 0x00 : 0x01)
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


async::result<void> Controller::_directTransfer(ControlTransfer info,
		QueueEntity *queue, size_t max_packet_size) {
	auto transaction = _buildControl(info.flags,
			info.setup, info.buffer, max_packet_size);
	_linkTransaction(queue, transaction);
	return transaction->voidPromise.async_get();
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
		active->voidPromise.set_value();

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
COFIBER_ROUTINE(cofiber::no_future, Controller::resetPort(int number), ([=] {
	auto offset = _space.load(cap_regs::caplength);
	auto port_space = _space.subspace(offset + 0x44 + (4 * number));

//	std::cout << "ehci: Port reset." << std::endl;
	port_space.store(port_regs::sc, portsc::portReset(true));

	uint64_t tick;
	HEL_CHECK(helGetClock(&tick));

	helix::AwaitClock await_clock;
	auto &&submit = helix::submitAwaitClock(&await_clock, tick + 50'000'000,
			helix::Dispatcher::global());
	COFIBER_AWAIT submit.async_wait();
	HEL_CHECK(await_clock.error());

	port_space.store(port_regs::sc, portsc::portReset(false));

//	std::cout << "ehci: Waiting for reset to complete." << std::endl;
	arch::bit_value<uint32_t> sc{0};
	do {
		sc = port_space.load(port_regs::sc);
	} while(sc & portsc::portReset);

	if(sc & portsc::enableStatus) {
		assert(!(sc & portsc::enableChange)); // See handleIrqs().
		std::cout << "ehci: Port " << number << " was enabled." << std::endl;
		_enumerator.enablePort(number);
	}else{
		// TODO: We should grant the port to the companion controller here.
		std::cout << "ehci: Port " << number << " disabled after reset." << std::endl;
	}
}))

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
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, bindController(mbus::Entity entity), ([=] {
	protocols::hw::Device device(COFIBER_AWAIT entity.bind());
	auto info = COFIBER_AWAIT device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = COFIBER_AWAIT device.accessBar(0);
	auto irq = COFIBER_AWAIT device.accessIrq();

	helix::Mapping mapping{bar, info.barInfo[0].offset, info.barInfo[0].length};
	auto controller = std::make_shared<Controller>(std::move(device), std::move(mapping),
			std::move(bar), std::move(irq));
	controller->initialize();
	globalControllers.push_back(std::move(controller));
}))

COFIBER_ROUTINE(cofiber::no_future, observeControllers(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-class", "0c"),
		mbus::EqualsFilter("pci-subclass", "03"),
		mbus::EqualsFilter("pci-interface", "20")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		std::cout << "ehci: Detected controller" << std::endl;
		bindController(std::move(entity));
	});

	COFIBER_AWAIT root.linkObserver(std::move(filter), std::move(handler));
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "ehci: Starting driver";
	std::cout << " (compiled on " __DATE__ " " __TIME__ ")" << std::endl;

//	HEL_CHECK(helSetPriority(kHelThisThread, 2));

	{
		async::queue_scope scope{helix::globalQueue()};
		observeControllers();
	}

	helix::globalQueue()->run();

	return 0;
}
 
