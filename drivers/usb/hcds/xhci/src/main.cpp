
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <optional>
#include <functional>
#include <memory>
#include <bit>
#include <format>

#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/kernlet/compiler.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/hub.hpp>
#include <protocols/usb/server.hpp>

#include <helix/memory.hpp>

#include "spec.hpp"
#include "context.hpp"
#include "xhci.hpp"
#include "trb.hpp"

namespace proto = protocols::usb;

// ----------------------------------------------------------------
// Controller
// ----------------------------------------------------------------

std::vector<std::shared_ptr<Controller>> globalControllers;

Controller::Controller(protocols::hw::Device hw_device, mbus_ng::Entity entity, helix::Mapping mapping,
		helix::UniqueDescriptor mmio, helix::UniqueIrq irq, std::string name)
: _hw_device{std::move(hw_device)}, _mapping{std::move(mapping)},
		_mmio{std::move(mmio)}, _irq{std::move(irq)},
		_space{_mapping.get()}, _name{name}, _memoryPool{},
		_dcbaa{&_memoryPool, 256}, _cmdRing{this},
		_eventRing{this},
		_enumerator{this}, _largeCtx{false},
		_entity{std::move(entity)} {
	auto doorbell_offset = _space.load(cap_regs::dboff);
	_doorbells = _space.subspace(doorbell_offset);

	_numPorts = _space.load(cap_regs::hcsparams1) & hcsparams1::maxPorts;
	_ports.resize(_numPorts);
	std::cout << this << _numPorts << " ports in total" << std::endl;
}

void Controller::_processExtendedCapabilities() {
	auto cur = (_space.load(cap_regs::hccparams1) & hccparams1::extCapPtr) * 4u;
	if(!cur)
		return;

	while(1) {
		auto val = arch::scalar_load<uint32_t>(_space, cur);
		if(val == 0xFFFFFFFF)
			break;

		auto id = val & 0xFF;
		if(!id)
			break;

		if(id == 1) {
			std::cout << this << "USB Legacy Support capability at " << cur << std::endl;

			while(arch::scalar_load<uint8_t>(_space, cur + 0x2)) {
				arch::scalar_store<uint8_t>(_space, cur + 0x3, 1);
				sleep(1);
			}

			std::cout << this << "Controller ownership obtained from BIOS" << std::endl;
		} else if(id == 2) {
			SupportedProtocol proto;

			auto v = arch::scalar_load<uint32_t>(_space, cur);
			proto.major = (v >> 24) & 0xFF;
			proto.minor = (v >> 16) & 0xFF;

			v = arch::scalar_load<uint32_t>(_space, cur + 8);
			proto.compatiblePortStart = v & 0xFF;
			proto.compatiblePortCount = (v >> 8) & 0xFF;

			v = arch::scalar_load<uint32_t>(_space, cur + 12);
			proto.slotType = v & 0xF;

			_supportedProtocols.push_back(proto);
		}

		auto next = cur + (((val >> 8) & 0xFF) << 2);
		if (next == cur)
			break;

		cur = next;
	}
}

async::detached Controller::initialize() {
	auto opOffset = _space.load(cap_regs::caplength);
	auto operational = _space.subspace(opOffset);

	_processExtendedCapabilities();

	std::cout << this << "Initializing controller" << std::endl;

	// Stop the controller
	operational.store(op_regs::usbcmd, usbcmd::run(false));

	// Wait for the controller to halt
	while(!(operational.load(op_regs::usbsts) & usbsts::hcHalted))
		;

	// Reset the controller and wait for it to complete
	operational.store(op_regs::usbcmd, usbcmd::hcReset(1));
	while(operational.load(op_regs::usbsts) & usbsts::controllerNotReady)
		;

	std::cout << this << "Controller reset done" << std::endl;

	_largeCtx = _space.load(cap_regs::hccparams1) & hccparams1::contextSize;

	_maxDeviceSlots = _space.load(cap_regs::hcsparams1) & hcsparams1::maxDevSlots;
	operational.store(op_regs::config, config::enabledDeviceSlots(_maxDeviceSlots));

	// Figure out how many scratchpad buffers are needed
	auto nScratchpadBufs =
		(uint32_t{_space.load(cap_regs::hcsparams2) & hcsparams2::maxScratchpadBufsHi} << 5)
		| (uint32_t{_space.load(cap_regs::hcsparams2) & hcsparams2::maxScratchpadBufsLow});
	std::cout << this << "Controller wants " << nScratchpadBufs << " scratchpad buffers" << std::endl;

	// Pick the smallest supported page size
	// XXX(qookie): Linux seems to not care and always uses 4K
	// pages? I can't find anything in the spec that justifies
	// doing that...
	auto pageSize = 1u << ((std::countr_zero(operational.load(op_regs::pagesize))) + 12);
	std::cout << this << "Controller's minimum page size is " << pageSize << std::endl;

	// Allocate the scratchpad buffers
	_scratchpadBufArray = arch::dma_array<uint64_t>{&_memoryPool, nScratchpadBufs};
	for (size_t i = 0; i < nScratchpadBufs; i++) {
		_scratchpadBufs.push_back(arch::dma_buffer(&_memoryPool,
					pageSize));

		_scratchpadBufArray[i] = helix::ptrToPhysical(_scratchpadBufs.back().data());
	}

	// Initialize the device context pointer array
	for (size_t i = 0; i < 256; i++)
		_dcbaa[i] = 0;
	_dcbaa[0] = helix::ptrToPhysical(_scratchpadBufArray.data());

	// Tell the controller about our device context pointer array
	operational.store(op_regs::dcbaap, helix::ptrToPhysical(_dcbaa.data()));

	// Tell the controller about our command ring
	operational.store(op_regs::crcr, _cmdRing.getPtr() | 1);

	// Set up interrupters
	// TODO(qookie): MSIs let us use multiple interrupters to
	// spread out the load (we probably want up to 1 per core?)
	auto runtimeOffset = _space.load(cap_regs::rtsoff);
	auto runtime = _space.subspace(runtimeOffset);
	_interrupters.push_back(std::make_unique<Interrupter>(
				&_eventRing,
				interrupter::interrupterSpace(runtime, 0)));
	_interrupters.back()->handleIrqs(_irq);
	_interrupters.back()->initialize();

	// Start the controller and enable interrupts
	operational.store(op_regs::usbcmd, usbcmd::run(1) | usbcmd::intrEnable(1));

	// Wait for the controller to start
	while(operational.load(op_regs::usbsts) & usbsts::hcHalted)
		;

	// Set up root hubs for each protocol
	for (auto &p : _supportedProtocols) {
		std::cout << this << "USB " << std::format("{:x}.{:02x}", p.major, p.minor)
			<< ": " << p.compatiblePortCount << " ports ("
			<< p.compatiblePortStart << "-" << (p.compatiblePortStart + p.compatiblePortCount - 1)
			<< "), slot type " << p.slotType << std::endl;

		mbus_ng::Properties descriptor{
			{"generic.devtype", mbus_ng::StringItem{"usb-controller"}},
			{"generic.devsubtype", mbus_ng::StringItem{"xhci"}},
			{"usb.version.major", mbus_ng::StringItem{std::to_string(p.major)}},
			{"usb.version.minor", mbus_ng::StringItem{std::to_string(p.minor)}},
			{"usb.root.parent", mbus_ng::StringItem{std::to_string(_entity.id())}},
		};

		auto xhciEntity = (co_await mbus_ng::Instance::global().createEntity(
					"xhci-controller", descriptor)).unwrap();

		auto hub = std::make_shared<RootHub>(
			this, p,
			op_regs::portSpace(operational, p.compatiblePortStart - 1),
			std::move(xhciEntity));
		_rootHubs.push_back(hub);
		_enumerator.observeHub(hub);
	}

	std::cout << this << "Initialization done" << std::endl;
}

void Controller::ringDoorbell(uint8_t doorbell, uint8_t target, uint16_t stream_id) {
	arch::scalar_store<uint32_t>(_doorbells, doorbell * 4,
			target | (stream_id << 16));
}


async::result<frg::expected<proto::UsbError>>
Controller::enumerateDevice(std::shared_ptr<proto::Hub> parentHub, int port, proto::DeviceSpeed speed) {
	uint32_t route = 0;
	size_t rootPort = port;

	if (parentHub->parent()) {
		route |= port > 14 ? 14 : (port + 1);
	}

	std::shared_ptr<proto::Hub> h = parentHub;

	while (h->parent()) {
		if (h->parent()->parent()) {
			int port = h->parent()->port();

			route <<= 4;
			route |= port > 14 ? 14 : (port + 1);
		}

		h = h->parent();
	}

	if (parentHub->parent()) {
		rootPort = h->port();
	}

	SupportedProtocol *proto = std::static_pointer_cast<RootHub>(h)->protocol();

	rootPort += proto->compatiblePortStart;

	auto device = std::make_shared<Device>(this);
	FRG_CO_TRY(co_await device->enumerate(rootPort, port, route, parentHub, speed, proto->slotType));
	_devices[device->slot()] = device;

	// If this is full speed, our guess for MPS might be wrong,
	// get the first 8 bytes of the device descriptor to check.
	if (speed == proto::DeviceSpeed::fullSpeed) {
		arch::dma_object<proto::DeviceDescriptor> descriptor{&_memoryPool};
		FRG_CO_TRY(co_await device->readDescriptor(descriptor.view_buffer().subview(0, 8), 0x0100));

		std::cout << this << "Full-speed device on port " << port
			<< " has bMaxPacketSize0 = " << int{descriptor->maxPacketSize} << std::endl;

		FRG_CO_TRY(co_await device->updateEp0PacketSize(descriptor->maxPacketSize));
	}

	arch::dma_object<proto::DeviceDescriptor> descriptor{&_memoryPool};
	FRG_CO_TRY(co_await device->readDescriptor(descriptor.view_buffer(), 0x0100));

	// Advertise the USB device on mbus.
	char class_code[3], sub_class[3], protocol[3];
	char vendor[5], product[5], release[5];
	snprintf(class_code, 3, "%.2x", descriptor->deviceClass);
	snprintf(sub_class, 3, "%.2x", descriptor->deviceSubclass);
	snprintf(protocol, 3, "%.2x", descriptor->deviceProtocol);
	snprintf(vendor, 5, "%.4x", descriptor->idVendor);
	snprintf(product, 5, "%.4x", descriptor->idProduct);
	snprintf(release, 5, "%.4x", descriptor->bcdDevice);

	if (descriptor->deviceClass == 0x09 && descriptor->deviceSubclass == 0) {
		auto hub = FRG_CO_TRY(co_await createHubFromDevice(parentHub, proto::Device{device}, port));

		FRG_CO_TRY(co_await device->configureHub(hub, speed));

		_enumerator.observeHub(std::move(hub));
	}

	char name[3];
	snprintf(name, 3, "%.2lx", device->slot());

	std::string mbps = protocols::usb::getSpeedMbps(speed);

	auto entity_id = std::static_pointer_cast<RootHub>(h)->entityId();

	mbus_ng::Properties mbusDescriptor{
		{"usb.type", mbus_ng::StringItem{"device"}},
		{"usb.vendor", mbus_ng::StringItem{vendor}},
		{"usb.product", mbus_ng::StringItem{product}},
		{"usb.class", mbus_ng::StringItem{class_code}},
		{"usb.subclass", mbus_ng::StringItem{sub_class}},
		{"usb.protocol", mbus_ng::StringItem{protocol}},
		{"usb.release", mbus_ng::StringItem{release}},
		{"usb.hub_port", mbus_ng::StringItem{name}},
		{"usb.bus", mbus_ng::StringItem{std::to_string(entity_id)}},
		{"usb.speed", mbus_ng::StringItem{mbps}},
		{"unix.subsystem", mbus_ng::StringItem{"usb"}},
	};

	auto usbEntity = (co_await mbus_ng::Instance::global().createEntity(
				"usb-xhci-dev-" + std::string{name}, mbusDescriptor)).unwrap();

	[] (auto device, mbus_ng::EntityManager entity) -> async::detached {
		while (true) {
			auto [localLane, remoteLane] = helix::createStream();

			// If this fails, too bad!
			(void)(co_await entity.serveRemoteLane(std::move(remoteLane)));

			proto::serve(proto::Device{device}, std::move(localLane));
		}
	}(device, std::move(usbEntity));

	co_return frg::success;
}

void Controller::processEvent(Event ev) {
	switch (ev.type) {
		using enum TrbType;

		case commandCompletionEvent:
			_cmdRing.processEvent(ev);
			break;

		case transferEvent:
			if (auto ep = _devices[ev.slotId]->endpoint(ev.endpointId))
				ep->transferRing().processEvent(ev);
			else
				std::cout << this << "Event for missing endpoint ID " << ev.endpointId
					<< " on slot " << ev.slotId << std::endl;
			break;

		case portStatusChangeEvent:
			assert(ev.portId <= _ports.size());
			if (_ports[ev.portId - 1])
				_ports[ev.portId - 1]->_doorbell.raise();
			break;

		default:
			std::cout << this << "Unexpected event in processEvent, ignoring..." << std::endl;
			ev.printInfo();
	}
}

// ------------------------------------------------------------------------
// Interrutper
// ------------------------------------------------------------------------

void Interrupter::initialize() {
	// Initialize the event ring segment table
	_space.store(interrupter::erstsz, _ring->getErstSize());
	_space.store(interrupter::erstbaLow,_ring->getErstPtr() & 0xFFFFFFFF);
	_space.store(interrupter::erstbaHi, _ring->getErstPtr() >> 32);

	_updateDequeue();

	_space.store(interrupter::iman, _space.load(interrupter::iman) | iman::enable(1));
}

async::detached Interrupter::handleIrqs(helix::UniqueIrq &irq) {
	uint64_t sequence = 0;

	while(1) {
		auto await = co_await helix_ng::awaitEvent(irq, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		if (!_isBusy()) {
			HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckNack, sequence));
			continue;
		}

		_clearPending();
		HEL_CHECK(helAcknowledgeIrq(irq.getHandle(), kHelAckAcknowledge, sequence));

		_ring->processRing();
		_updateDequeue();
	}
}

void Interrupter::_updateDequeue() {
	_space.store(interrupter::erdpLow,
			(_ring->getEventRingPtr() & 0xFFFFFFF0) | (1 << 3));
	_space.store(interrupter::erdpHi, _ring->getEventRingPtr() >> 32);
}

bool Interrupter::_isBusy() {
	return _space.load(interrupter::erdpLow) & (1 << 3);
}

void Interrupter::_clearPending() {
	_space.store(interrupter::iman, _space.load(interrupter::iman) | iman::pending(1));
}

// ------------------------------------------------------------------------
// Controller::Port
// ------------------------------------------------------------------------

Controller::Port::Port(int id, arch::mem_space space, Controller *controller, SupportedProtocol *proto)
: _id{id}, _controller{controller}, _proto{proto}, _space{space} {
}

void Controller::Port::reset() {
	std::cout << _controller << "Resetting port " << _id << std::endl;
	_space.store(port::portsc, portsc::portPower(true) | portsc::portReset(true));
}

void Controller::Port::disable() {
	_space.store(port::portsc, portsc::portPower(true) | portsc::portEnable(true));
}

void Controller::Port::resetChangeBits() {
	_space.store(port::portsc, portsc::portPower(true)
			| portsc::connectStatusChange(true)
			| portsc::portResetChange(true)
			| portsc::portEnableChange(true)
			| portsc::warmPortResetChange(true)
			| portsc::overCurrentChange(true)
			| portsc::portLinkStatusChange(true)
			| portsc::portConfigErrorChange(true));
}

bool Controller::Port::isConnected() {
	auto portsc = _space.load(port::portsc);
	return portsc & portsc::connectStatus;
}

bool Controller::Port::isPowered() {
	auto portsc = _space.load(port::portsc);
	return portsc & portsc::portPower;
}

bool Controller::Port::isEnabled() {
	return _space.load(port::portsc) & portsc::portEnable;
}

uint8_t Controller::Port::getLinkStatus() {
	return _space.load(port::portsc) & portsc::portLinkStatus;
}

uint8_t Controller::Port::getSpeed() {
	return _space.load(port::portsc) & portsc::portSpeed;
}

void Controller::Port::transitionToLinkStatus(uint8_t status) {
	_space.store(port::portsc, portsc::portPower(true)
			| portsc::portLinkStatus(status)
			| portsc::portLinkStatusStrobe(true));
}

async::detached Controller::Port::initPort() {
	if (!isPowered()) {
		std::cout << _controller << "Port " << _id << " is not powered on" << std::endl;
	}

	// Wait for something to connect to the port
	co_await awaitFlag(portsc::connectStatus, true);

	// Notify the enumerator
	_state.changes |= proto::HubStatus::connect;
	_state.status |= proto::HubStatus::connect;
	_pollEv.raise();
}

async::result<proto::PortState> Controller::Port::pollState() {
	_pollSeq = co_await _pollEv.async_wait(_pollSeq);
	co_return _state;
}

async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> Controller::Port::issueReset() {
	// We know something is connected if we're here (CCS=1)

	// Reset the port only for USB 2 devices.
	// "A USB3 protocol port attempts to automatically advance to
	//  the Enabled state as part of the attach process."
	// "A USB2 protocol port requires software to reset the port
	//  to advance the port to the Enabled state [...]"
	if (_proto->major == 2) {
		reset();
	}

	// Wait for the port to enable.
	co_await awaitFlag(portsc::portEnable, true);

	auto linkStatus = getLinkStatus();

	std::cout << _controller << "Port " << _id << " link status is " << (unsigned int)linkStatus << std::endl;

	if (linkStatus >= 1 && linkStatus <= 3) {
		transitionToLinkStatus(0);
	} else {
		assert(linkStatus == 0); // U0
	}

	// Notify the enumerator.
	_state.changes |= proto::HubStatus::enable;
	_state.status |= proto::HubStatus::enable;
	_pollEv.raise();

	// Figure out the device speed.

	uint8_t speedId = getSpeed();

	std::optional<proto::DeviceSpeed> speed;

	switch(speedId) {
		case 1:
			speed = proto::DeviceSpeed::fullSpeed;
			break;
		case 2:
			speed = proto::DeviceSpeed::lowSpeed;
			break;
		case 3:
			speed = proto::DeviceSpeed::highSpeed;
			break;
		case 4:
		case 5:
		case 6:
		case 7:
			speed = proto::DeviceSpeed::superSpeed;
	}

	if (speed) {
		co_return speed.value();
	} else {
		std::cout << _controller << "Port " << _id << " has invalid speed ID " << speedId << std::endl;
		co_return proto::UsbError::unsupported;
	}
}

// ------------------------------------------------------------------------
// Controller::RootHub
// ------------------------------------------------------------------------

Controller::RootHub::RootHub(Controller *controller, SupportedProtocol &proto, arch::mem_space portSpace, mbus_ng::EntityManager entity)
: Hub{nullptr, 0}, _controller{controller}, _proto{&proto}, _entity{std::move(entity)} {
	for (size_t i = 0; i < proto.compatiblePortCount; i++) {
		_ports.push_back(std::make_unique<Port>(
					i + proto.compatiblePortStart,
					port::spaceForIndex(portSpace, i),
					controller, &proto));
		_ports.back()->initPort();
		_controller->_ports[(i + proto.compatiblePortStart - 1)] =
				_ports.back().get();
	}
}

size_t Controller::RootHub::numPorts() {
	return _proto->compatiblePortCount;
}

async::result<proto::PortState> Controller::RootHub::pollState(int port) {
	co_return co_await _ports[port]->pollState();
}

async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> Controller::RootHub::issueReset(int port) {
	co_return FRG_CO_TRY(co_await _ports[port]->issueReset());
}

// ------------------------------------------------------------------------
// Device
// ------------------------------------------------------------------------

Device::Device(Controller *controller)
: _slotId{-1}, _controller{controller} {
}

arch::dma_pool *Device::setupPool() {
	return _controller->memoryPool();
}

arch::dma_pool *Device::bufferPool() {
	return _controller->memoryPool();
}

async::result<frg::expected<proto::UsbError, std::string>>
Device::deviceDescriptor() {
	arch::dma_object<proto::DeviceDescriptor> descriptor{bufferPool()};
	FRG_CO_TRY(co_await readDescriptor(descriptor.view_buffer(), 0x0100));
	co_return std::string{(char *)descriptor.data(), descriptor.view_buffer().size()};
}

async::result<frg::expected<proto::UsbError, std::string>>
Device::configurationDescriptor(uint8_t configuration) {
	arch::dma_object<proto::ConfigDescriptor> header{bufferPool()};
	FRG_CO_TRY(co_await readDescriptor(header.view_buffer(), 0x0200 | configuration));

	arch::dma_buffer descriptor{bufferPool(), header->totalLength};
	FRG_CO_TRY(co_await readDescriptor(descriptor, 0x0200 | configuration));
	co_return std::string{(char *)descriptor.data(), descriptor.size()};
}

async::result<frg::expected<proto::UsbError, proto::Configuration>>
Device::useConfiguration(uint8_t index, uint8_t value) {
	auto descriptor = FRG_CO_TRY(co_await configurationDescriptor(index));

	struct EndpointInfo {
		int pipe;
		proto::PipeType dir;
		int packetSize;
		proto::EndpointType type;
	};

	std::vector<EndpointInfo> _eps = {};

	std::optional<uint8_t> valueByIndex;

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
		auto packetSize = desc->maxPacketSize & 0x7FF;
		auto epType = info.endpointType.value();

		int pipe = info.endpointNumber.value();
		if (info.endpointIn.value()) {
			_eps.push_back({pipe, proto::PipeType::in, packetSize, epType});
		} else {
			_eps.push_back({pipe, proto::PipeType::out, packetSize, epType});
		}
	});

	assert(valueByIndex);
	// Bail out if the user has no idea what they're asking for
	if (*valueByIndex != value) {
		std::cout << _controller << "useConfiguration(" << (unsigned int)index << ", " << (unsigned int)value
			<< ") called, but that configuration has bConfigurationValue = " << (unsigned int)*valueByIndex << "???" << std::endl;
		co_return proto::UsbError::other;
	}

	for (auto &ep : _eps) {
		std::cout << _controller << "Setting up " << (ep.dir == proto::PipeType::in ? "in" : "out")
			<< " endpoint " << ep.pipe << " (max packet size: " << ep.packetSize << ")" << std::endl;

		FRG_CO_TRY(co_await setupEndpoint(ep.pipe, ep.dir, ep.packetSize, ep.type));
	}

	arch::dma_object<proto::SetupPacket> setConfig{setupPool()};
	setConfig->type = proto::setup_type::targetDevice | proto::setup_type::byStandard | proto::setup_type::toDevice;
	setConfig->request = proto::request_type::setConfig;
	setConfig->value = value;
	setConfig->index = 0;
	setConfig->length = 0;

	FRG_CO_TRY(co_await transfer({protocols::usb::kXferToDevice, setConfig, {}}));

	std::cout << _controller << "Configuration set" << std::endl;

	co_return proto::Configuration{std::make_shared<ConfigurationState>(shared_from_this())};
}

async::result<frg::expected<proto::UsbError, size_t>>
Device::transfer(proto::ControlTransfer info) {
	co_return co_await _endpoints[0]->transfer(info);
}

void Device::submit(int endpoint) {
	assert(_slotId != -1);
	_controller->ringDoorbell(_slotId, endpoint);
}

static inline uint8_t getHcdSpeedId(proto::DeviceSpeed speed) {
	switch (speed) {
		using enum proto::DeviceSpeed;

		case lowSpeed: return 2;
		case fullSpeed: return 1;
		case highSpeed: return 3;
		case superSpeed: return 4;
	}

	return 0;
}

async::result<frg::expected<proto::UsbError>>
Device::enumerate(size_t rootPort, size_t port, uint32_t route, std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed, int slotType) {
	auto event = co_await _controller->submitCommand(
			Command::enableSlot(slotType));

	FRG_CO_TRY(completionToError(event));

	assert(event.completionCode != 9); // TODO: handle running out of device slots
	assert(event.completionCode == 1); // success

	_slotId = event.slotId;

	std::cout << _controller << "Slot " << _slotId << " allocated for port " << port
		<< " (route " << std::hex << route << std::dec << ")" << std::endl;

	// initialize slot

	_devCtx = DeviceContext{_controller->largeCtx(), _controller->memoryPool()};

	InputContext inputCtx{_controller->largeCtx(), _controller->memoryPool()};
	auto &slotCtx = inputCtx.get(inputCtxSlot);

	inputCtx.get(inputCtxCtrl) |= InputControlFields::add(0); // Slot Context

	slotCtx |= SlotFields::routeString(route);
	slotCtx |= SlotFields::ctxEntries(1);
	slotCtx |= SlotFields::speed(getHcdSpeedId(speed));

	if ((speed == proto::DeviceSpeed::lowSpeed || speed == proto::DeviceSpeed::fullSpeed)
			&& hub->parent()) {
		// We need to fill these fields out for split transactions.

		auto hubDevice = std::static_pointer_cast<Device>(hub->associatedDevice()->state());

		slotCtx |= SlotFields::parentHubPort(hub->port() + 1);
		slotCtx |= SlotFields::parentHubSlot(hubDevice->_slotId);
	}

	slotCtx |= SlotFields::rootHubPort(rootPort);

	size_t packetSize = 0;
	switch (speed) {
		using enum proto::DeviceSpeed;

		case lowSpeed:
		case fullSpeed: packetSize = 8; break;
		case highSpeed: packetSize = 64; break;
		case superSpeed: packetSize = 512; break;
	}

	_initEpCtx(inputCtx, 0, proto::PipeType::control, packetSize, proto::EndpointType::control);

	_controller->setDeviceContext(_slotId, _devCtx);

	event = co_await _controller->submitCommand(
			Command::addressDevice(_slotId,
				helix::ptrToPhysical(inputCtx.rawData())));

	if (event.completionCode != 1)
		std::cout << _controller << "Failed to address device on slot " << _slotId
			<< ", completion code: " << completionCodeNames[event.completionCode] << std::endl;

	FRG_CO_TRY(completionToError(event));

	std::cout << _controller << "Device successfully addressed" << std::endl;

	co_return frg::success;
}

async::result<frg::expected<proto::UsbError>>
Device::readDescriptor(arch::dma_buffer_view dest, uint16_t desc) {
	arch::dma_object<proto::SetupPacket> getDesc{setupPool()};
	getDesc->type = proto::setup_type::targetDevice | proto::setup_type::byStandard | proto::setup_type::toHost;
	getDesc->request = proto::request_type::getDescriptor;
	getDesc->value = desc;
	getDesc->index = 0;
	getDesc->length = dest.size();

	FRG_CO_TRY(co_await transfer({protocols::usb::kXferToHost, getDesc, dest}));

	co_return frg::success;
}

static inline uint32_t getHcdEndpointType(proto::PipeType dir, proto::EndpointType type) {
	using proto::EndpointType;
	using proto::PipeType;

	if (type == EndpointType::control)
		return 4;
	if (type == EndpointType::isochronous)
		return 1 + (dir == PipeType::in ? 4 : 0);
	if (type == EndpointType::bulk)
		return 2 + (dir == PipeType::in ? 4 : 0);
	if (type == EndpointType::interrupt)
		return 3 + (dir == PipeType::in ? 4 : 0);
	return 0;
}

static inline uint32_t getDefaultAverageTrbLen(proto::EndpointType type) {
	using proto::EndpointType;

	if (type == EndpointType::control)
		return 8;
	if (type == EndpointType::isochronous)
		return 3 * 1024;
	if (type == EndpointType::bulk)
		return 3 * 1024;
	if (type == EndpointType::interrupt)
		return 1 * 1024;
	return 0;
}



async::result<frg::expected<proto::UsbError>>
Device::setupEndpoint(int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type) {
	InputContext inputCtx{_controller->largeCtx(), _controller->memoryPool()};

	inputCtx.get(inputCtxCtrl) |= InputControlFields::add(0); // Slot Context
	inputCtx.get(inputCtxSlot) = _devCtx.get(deviceCtxSlot);
	inputCtx.get(inputCtxSlot) |= SlotFields::ctxEntries(31);

	_initEpCtx(inputCtx, endpoint, dir, maxPacketSize, type);

	auto event = co_await _controller->submitCommand(
			Command::configureEndpoint(_slotId,
				helix::ptrToPhysical(inputCtx.rawData())));

	if (event.completionCode != 1)
		std::cout << _controller << "Failed to configure endpoint " << endpoint
			<< ", completion code: " << completionCodeNames[event.completionCode] << std::endl;

	FRG_CO_TRY(completionToError(event));

	std::cout << _controller << "Endpoint " << endpoint << " configured" << std::endl;

	co_return frg::success;
}

async::result<frg::expected<proto::UsbError>>
Device::configureHub(std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed) {
	InputContext inputCtx{_controller->largeCtx(), _controller->memoryPool()};

	inputCtx.get(inputCtxCtrl) |= InputControlFields::add(0); // Slot Context
	inputCtx.get(inputCtxSlot) = _devCtx.get(deviceCtxSlot);

	inputCtx.get(inputCtxSlot) |= SlotFields::hub(true);
	inputCtx.get(inputCtxSlot) |= SlotFields::portCount(hub->numPorts());

	if (speed == proto::DeviceSpeed::highSpeed)
		inputCtx.get(inputCtxSlot) |= SlotFields::ttThinkTime(
				hub->getCharacteristics().unwrap().ttThinkTime / 8 - 1);

	auto event = co_await _controller->submitCommand(
			Command::evaluateContext(_slotId,
				helix::ptrToPhysical(inputCtx.rawData())));

	if (event.completionCode != 1)
		std::cout << _controller << "Failed to evaluate context for slot " << _slotId
			<< ", completion code: " << completionCodeNames[event.completionCode] << std::endl;

	FRG_CO_TRY(completionToError(event));

	std::cout << _controller << "Hub setup done" << std::endl;

	co_return frg::success;
}

void Device::_initEpCtx(InputContext &ctx, int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type) {
	int endpointId = getEndpointIndex(endpoint, dir);

	ctx.get(inputCtxCtrl) |= InputControlFields::add(endpointId); // EP Context

	auto ep = std::make_shared<EndpointState>(this, endpointId, type, maxPacketSize);
	_endpoints[endpointId - 1] = ep;

	auto trPtr = ep->transferRing().getPtr();

	auto &epCtx = ctx.get(inputCtxEp0 + endpointId - 1);

	epCtx |= EpFields::errorCount(3);
	// TODO(qookie): Compute this from bInterval, 6 should be a safe guess:
	// 2**6 * 125us = 8000us (=> 125Hz polling rate).
	epCtx |= EpFields::interval(6);
	epCtx |= EpFields::epType(getHcdEndpointType(dir, type));
	epCtx |= EpFields::maxPacketSize(maxPacketSize);
	// TODO(qookie): This is fine for USB 2 (unless max burst > 0),
	// but for USB 3 this should use wBytesPerInterval from the SS
	// endpoint companion descriptor.
	epCtx |= EpFields::maxEsitPayloadLo(maxPacketSize);
	epCtx |= EpFields::maxEsitPayloadHi(maxPacketSize);
	epCtx |= EpFields::dequeCycle(true);
	epCtx |= EpFields::trPointerLo(trPtr);
	epCtx |= EpFields::trPointerHi(trPtr);

	// TODO(qookie): We should keep track of the average transfer sizes and
	// update this every once in a while. Currently we just use the recommended
	// initial values from the specification.
	epCtx |= EpFields::averageTrbLength(getDefaultAverageTrbLen(type));
}

async::result<frg::expected<proto::UsbError>>
Device::updateEp0PacketSize(size_t maxPacketSize) {
	InputContext inputCtx{_controller->largeCtx(), _controller->memoryPool()};
	int endpointId = getEndpointIndex(0, proto::PipeType::control);

	inputCtx.get(inputCtxCtrl) |= InputControlFields::add(endpointId); // EP Context

	auto &epCtx = inputCtx.get(inputCtxEp0 + endpointId - 1);
	epCtx = _devCtx.get(deviceCtxEp0 + endpointId - 1);

	epCtx &= ~EpFields::maxPacketSize(0xFFFF);
	epCtx |= EpFields::maxPacketSize(maxPacketSize);

	epCtx &= ~EpFields::maxEsitPayloadLo(0xFFFFFF);
	epCtx &= ~EpFields::maxEsitPayloadHi(0xFFFFFF);
	epCtx |= EpFields::maxEsitPayloadLo(maxPacketSize);
	epCtx |= EpFields::maxEsitPayloadHi(maxPacketSize);

	_endpoints[endpointId - 1]->_maxPacketSize = maxPacketSize;

	auto event = co_await _controller->submitCommand(
		Command::evaluateContext(_slotId,
				helix::ptrToPhysical(inputCtx.rawData())));

	if (event.completionCode != 1)
		std::cout << _controller << "Failed to evaluate context for slot " << _slotId
			<< ", completion code: " << completionCodeNames[event.completionCode] << std::endl;

	FRG_CO_TRY(completionToError(event));

	co_return frg::success;
}

// ------------------------------------------------------------------------
// ConfigurationState
// ------------------------------------------------------------------------

async::result<frg::expected<proto::UsbError, proto::Interface>>
ConfigurationState::useInterface(int number, int alternative) {
	arch::dma_object<proto::SetupPacket> desc{_device->setupPool()};
	desc->type = proto::setup_type::targetInterface | proto::setup_type::byStandard | proto::setup_type::toDevice;
	desc->request = proto::request_type::setInterface;
	desc->value = alternative;
	desc->index = number;
	desc->length = 0;

	// The device might stall if only the default setting is
	// supported so just ignore that.
	auto res = co_await _device->transfer({proto::kXferToDevice, desc, {}});
	if (!res && res.error() == proto::UsbError::stall) {
		std::cout << _device->controller() << "SET_INTERFACE(" << number << ", " << alternative
			<< ") stalled, ignoring..." << std::endl;
	} else {
		FRG_CO_TRY(res);
	}

	co_return proto::Interface{std::make_shared<InterfaceState>(_device, number)};
}

// ------------------------------------------------------------------------
// EndpointState
// ------------------------------------------------------------------------

async::result<frg::expected<proto::UsbError, size_t>>
EndpointState::transfer(proto::ControlTransfer info) {
	ProducerRing::Transaction tx;

	Transfer::buildControlChain([&] (RawTrb trb) {
		_transferRing.pushRawTrb(trb, &tx);
	}, *info.setup.data(), info.buffer, info.flags == proto::kXferToHost,
			_maxPacketSize);

	size_t nextDequeue = _transferRing.enqueuePtr();
	bool nextCycle = _transferRing.producerCycle();

	_device->submit(_endpointId);

	auto maybeResidue = co_await tx.control(info.buffer.size() != 0);

	if (!maybeResidue && maybeResidue.error() == proto::UsbError::stall) {
		auto res = co_await _resetAfterError(nextDequeue, nextCycle);
		if (!res) {
			std::cout << _device->controller() << "Failed to reset EP " << _endpointId
				<< " after stall: " << (int)res.error() << std::endl;
		}
	}

	co_return info.buffer.size() - FRG_CO_TRY(maybeResidue);
}

async::result<frg::expected<proto::UsbError, size_t>>
EndpointState::_bulkOrInterruptXfer(arch::dma_buffer_view buffer) {
	ProducerRing::Transaction tx;

	Transfer::buildNormalChain([&] (RawTrb trb) {
		_transferRing.pushRawTrb(trb, &tx);
	}, buffer, _maxPacketSize);

	size_t nextDequeue = _transferRing.enqueuePtr();
	bool nextCycle = _transferRing.producerCycle();

	_device->submit(_endpointId);

	auto maybeResidue = co_await tx.normal();

	if (!maybeResidue && maybeResidue.error() == proto::UsbError::stall) {
		auto res = co_await _resetAfterError(nextDequeue, nextCycle);
		if (!res) {
			std::cout << _device->controller() << "Failed to reset EP " << _endpointId
				<< " after stall: " << (int)res.error() << std::endl;
		}
	}

	co_return buffer.size() - FRG_CO_TRY(maybeResidue);
}

async::result<frg::expected<proto::UsbError, size_t>>
EndpointState::transfer(proto::InterruptTransfer info) {
	co_return co_await _bulkOrInterruptXfer(info.buffer);
}

async::result<frg::expected<proto::UsbError, size_t>>
EndpointState::transfer(proto::BulkTransfer info) {
	co_return co_await _bulkOrInterruptXfer(info.buffer);
}

async::result<frg::expected<proto::UsbError>>
EndpointState::_resetAfterError(size_t nextDequeue, bool cycle) {
	// Issue the Reset Endpoint command to reset the xHC state
	auto event = co_await _device->controller()->submitCommand(
		Command::resetEndpoint(_device->slot(), _endpointId));

	if (event.completionCode != 1)
		std::cout << _device->controller() << "Failed to reset EP " << _endpointId
			<< ", completion code: " << completionCodeNames[event.completionCode] << std::endl;

	FRG_CO_TRY(completionToError(event));

	// TODO(qookie): If behind a TT, and this is a control or bulk
	// EP, issue ClearFeature(CLEAR_TT_BUFFER)

	// If this is not a control EP, clear the halt on the device
	// side.
	// XXX(qookie): Linux has class drivers deal with this (but
	// does the rest of the handling, incl. clearing TT buffers,
	// in the xHCI driver).
	if (_type != proto::EndpointType::control) {
		arch::dma_object<proto::SetupPacket> clearHalt{_device->setupPool()};
		clearHalt->type = proto::setup_type::targetEndpoint | proto::setup_type::byStandard | proto::setup_type::toDevice;
		clearHalt->request = proto::request_type::clearFeature;
		clearHalt->value = proto::features::endpointHalt;
		clearHalt->index = _endpointId >> 1; // Our ID is EP no. * 2 + direction
		clearHalt->length = 0;

		FRG_CO_TRY(co_await _device->transfer({protocols::usb::kXferToDevice, clearHalt, {}}));
	}

	// Issue the Set TR Dequeue Pointer command to skip the failed
	// transfer
	auto dequeue = _transferRing.getPtr() + nextDequeue * sizeof(RawTrb);
	event = co_await _device->controller()->submitCommand(
		Command::setTransferRingDequeue(_device->slot(), _endpointId,
				dequeue | cycle));

	if (event.completionCode != 1)
		std::cout << _device->controller() << "Failed to set TR dequeue pointer"
			<< ", completion code: " << completionCodeNames[event.completionCode] << std::endl;

	FRG_CO_TRY(completionToError(event));

	// Ring the doorbell to restart the pipe
	_device->submit(_endpointId);

	co_return frg::success;
}

// ------------------------------------------------------------------------
// Freestanding PCI discovery functions.
// ------------------------------------------------------------------------

async::detached bindController(mbus_ng::Entity entity) {
	protocols::hw::Device device((co_await entity.getRemoteLane()).unwrap());
	auto info = co_await device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = co_await device.accessBar(0);

	helix::UniqueDescriptor irq;

	if (info.numMsis) {
		co_await device.enableMsi();
		irq = co_await device.installMsi(0);
	} else {
		co_await device.enableBusIrq();
		irq = co_await device.accessIrq();
	}

	co_await device.enableBusmaster();

	helix::Mapping mapping{bar, info.barInfo[0].offset, info.barInfo[0].length};

	auto controller = std::make_shared<Controller>(std::move(device), std::move(entity), std::move(mapping),
			std::move(bar), std::move(irq), std::format("pci.{:08x}", info.barInfo[0].address));
	controller->initialize();
	globalControllers.push_back(std::move(controller));
}

async::detached observeControllers() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"pci-class", "0c"},
		mbus_ng::EqualsFilter{"pci-subclass", "03"},
		mbus_ng::EqualsFilter{"pci-interface", "30"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::cout << "xhci: Detected controller" << std::endl;
			bindController(std::move(entity));
		}
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "xhci: Starting driver" << std::endl;

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}

