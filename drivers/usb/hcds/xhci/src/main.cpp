
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <optional>
#include <functional>
#include <memory>

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

// When set to true, the driver ignores any speeds defined in the supported protocols
// in favor of the default values defined in the XHCI spec. This behavior imitates
// what Linux is doing in it's driver.
inline constexpr bool ignoreControllerSpeeds = true;

// ----------------------------------------------------------------
// Controller
// ----------------------------------------------------------------

std::vector<std::shared_ptr<Controller>> globalControllers;

Controller::Controller(protocols::hw::Device hw_device, mbus_ng::Entity entity, helix::Mapping mapping,
		helix::UniqueDescriptor mmio, helix::UniqueIrq irq, bool useMsis)
: _hw_device{std::move(hw_device)}, _mapping{std::move(mapping)},
		_mmio{std::move(mmio)}, _irq{std::move(irq)},
		_space{_mapping.get()}, _memoryPool{},
		_dcbaa{&_memoryPool, 256}, _cmdRing{this},
		_eventRing{this}, _useMsis{useMsis},
		_enumerator{this}, _largeCtx{false},
		_entity{std::move(entity)} {
	auto op_offset = _space.load(cap_regs::caplength);
	auto runtime_offset = _space.load(cap_regs::rtsoff);
	auto doorbell_offset = _space.load(cap_regs::dboff);
	_operational = _space.subspace(op_offset);
	_runtime = _space.subspace(runtime_offset);
	_doorbells = _space.subspace(doorbell_offset);

	_numPorts = _space.load(cap_regs::hcsparams1) & hcsparams1::maxPorts;
	printf("xhci: %u ports\n", _numPorts);
}

std::vector<std::pair<uint8_t, uint16_t>> Controller::getExtendedCapabilityOffsets() {
	auto ptr = (_space.load(cap_regs::hccparams1) & hccparams1::extCapPtr) * 4;
	if (!ptr)
		return {};

	std::vector<std::pair<uint8_t, uint16_t>> caps = {};

	while(1) {
		auto val = arch::scalar_load<uint32_t>(_space, ptr);

		if (val == 0xFFFFFFFF)
			break;

		if (!(val & 0xFF))
			break;

		caps.push_back({val & 0xFF, ptr});

		auto old_ptr = ptr;
		ptr += ((val >> 8) & 0xFF) << 2;
		if (old_ptr == ptr)
			break;
	}

	return caps;
}

async::detached Controller::initialize() {
	auto caps = getExtendedCapabilityOffsets();

	auto usb_legacy_cap = std::find_if(caps.begin(), caps.end(),
			[](auto &a){
				return a.first == 0x1;
			});

	if(usb_legacy_cap != caps.end()) {
		auto usb_legacy_cap_off = usb_legacy_cap->second;
		printf("xhci: usb legacy capability at %04x\n", usb_legacy_cap_off);

		auto val = arch::scalar_load<uint8_t>(_space, usb_legacy_cap_off + 0x2);

		if (val)
			arch::scalar_store<uint8_t>(_space, usb_legacy_cap_off + 0x3, 1);

		while (1) {
			val = arch::scalar_load<uint8_t>(_space, usb_legacy_cap_off + 0x2);

			if(!val)
				break;

			sleep(1);
		}

		printf("xhci: device obtained from bios\n");
	} else {
		printf("xhci: no usb legacy support extended capability\n");
	}

	for (auto &c : caps) {
		if (c.first != 0x2)
			continue;

		auto off = c.second;

		SupportedProtocol proto;

		auto v = arch::scalar_load<uint32_t>(_space, off);
		proto.major = (v >> 24) & 0xFF;
		proto.minor = (v >> 16) & 0xFF;
		off += 4;

		v = arch::scalar_load<uint32_t>(_space, off);
		proto.name = {
			static_cast<char>(v & 0xFF),
			static_cast<char>((v >> 8) & 0xFF),
			static_cast<char>((v >> 16) & 0xFF),
			static_cast<char>((v >> 24) & 0xFF)
		};
		off += 4;

		v = arch::scalar_load<uint32_t>(_space, off);
		proto.compatiblePortStart = v & 0xFF;
		proto.compatiblePortCount = (v >> 8) & 0xFF;
		proto.protocolDefined = (v >> 16) & 0xFFF;
		auto speedIdCount = (v >> 28) & 0xF;
		off += 4;

		v = arch::scalar_load<uint32_t>(_space, off);
		proto.protocolSlotType = v & 0xF;
		off += 4;

		proto.speeds = {};

		for (size_t i = 0; i < speedIdCount; i++) {
			v = arch::scalar_load<uint32_t>(_space, off);

			SupportedProtocol::PortSpeed speed;
			speed.value = v & 0xF;
			speed.exponent = (v >> 4) & 0x3;
			speed.type = (v >> 6) & 0x3;
			speed.fullDuplex = (v >> 8) & 1;
			speed.linkProtocol = (v >> 14) & 0x3;
			speed.mantissa = (v >> 16) & 0xFFFF;

			off += 4;

			proto.speeds.push_back(speed);
		}

		_supportedProtocols.push_back(proto);
	}

	for (auto &p : _supportedProtocols) {
		printf("xhci: supported protocol:\n");
		printf("xhci: name: \"%s\" %u.%u\n", p.name.c_str(), p.major, p.minor);
		printf("xhci: compatible ports: %lu to %lu\n", p.compatiblePortStart,
				p.compatiblePortStart + p.compatiblePortCount - 1);
		printf("xhci: protocol defined: %03x\n", p.protocolDefined);
		printf("xhci: protocol slot type: %lu\n", p.protocolSlotType);

		constexpr const char *exponent[] = {
			"B/s",
			"Kb/s",
			"Mb/s",
			"Gb/s"
		};

		constexpr const char *type[] = {
			"Symmetric",
			"Reserved",
			"Asymmetric Rx",
			"Asymmetric Tx"
		};

		constexpr const char *linkProtocol[] = {
			"SuperSpeed",
			"SuperSpeedPlus",
			"Reserved",
			"Reserved"
		};

		printf("xhci: supported speeds:\n");
		for (auto &s : p.speeds) {
			printf("xhci:\tspeed:%u %s\n", s.mantissa,
					exponent[s.exponent]);
			printf("xhci:\tfull duplex? %s\n",
					s.fullDuplex ? "yes" : "no");
			printf("xhci:\ttype: %s\n", type[s.type]);
			if (p.major == 3)
				printf("xhci:\tlink protocol: %s\n",
						linkProtocol[s.linkProtocol]);
		}
	}

	printf("xhci: initializing controller...\n");

	auto state = _operational.load(op_regs::usbcmd);
	state &= ~usbcmd::run;
	_operational.store(op_regs::usbcmd, state);

	while(!(_operational.load(op_regs::usbsts) & usbsts::hcHalted)); // wait for halt

	_operational.store(op_regs::usbcmd, usbcmd::hcReset(1)); // reset hcd
	while(_operational.load(op_regs::usbsts) & usbsts::controllerNotReady); // poll for reset to complete
	printf("xhci: controller reset done...\n");

	_largeCtx = _space.load(cap_regs::hccparams1) & hccparams1::contextSize;

	_maxDeviceSlots = _space.load(cap_regs::hcsparams1) & hcsparams1::maxDevSlots;
	_operational.store(op_regs::config, config::enabledDeviceSlots(_maxDeviceSlots));

	uint32_t hcsparams2 = static_cast<uint32_t>(_space.load(cap_regs::hcsparams2));
	uint32_t max_scratchpad_bufs = ((((hcsparams2) >> 16) & 0x3e0) | (((hcsparams2) >> 27) & 0x1f));

	auto pagesize_reg = _operational.load(op_regs::pagesize);
	size_t page_size = 1 << ((__builtin_ffs(pagesize_reg) - 1) + 12); // 2^(n + 12)

	printf("xhci: max scratchpad buffers: %u\n", max_scratchpad_bufs);
	printf("xhci: page size: %lu\n", page_size);

	auto max_erst = _space.load(cap_regs::hcsparams2) & hcsparams2::erstMax;
	max_erst = 1 << (max_erst);
	printf("xhci: max_erst: %u\n", max_erst);

	_scratchpadBufArray = arch::dma_array<uint64_t>{
		&_memoryPool, static_cast<size_t>(max_scratchpad_bufs)};
	for (size_t i = 0; i < max_scratchpad_bufs; i++) {
		_scratchpadBufs.push_back(arch::dma_buffer(&_memoryPool,
					page_size));

		_scratchpadBufArray[i] = helix::ptrToPhysical(_scratchpadBufs.back().data());
	}

	for (size_t i = 0; i < 256; i++)
		_dcbaa[i] = 0;

	_dcbaa[0] = helix::ptrToPhysical(_scratchpadBufArray.data());

	_operational.store(op_regs::dcbaap, helix::ptrToPhysical(_dcbaa.data())); // tell the device about our dcbaa

	_operational.store(op_regs::crcr, _cmdRing.getPtr() | 1);

	printf("xhci: setting up interrupters\n");
	auto max_intrs = _space.load(cap_regs::hcsparams1) & hcsparams1::maxIntrs;
	printf("xhci: max interrupters: %u\n", max_intrs);

	_interrupters.push_back(std::make_unique<Interrupter>(0, this));

	for (int i = 1; i < max_intrs; i++)
		_interrupters.push_back(std::make_unique<Interrupter>(i, this));

	_interrupters[0]->setEventRing(&_eventRing);
	_interrupters[0]->setEnable(true);

	if (_useMsis) {
		handleMsis();
	} else {
		co_await _hw_device.enableBusIrq();
		handleIrqs();
	}

	_ports.resize(_numPorts);

	_operational.store(op_regs::usbcmd, usbcmd::run(1) | usbcmd::intrEnable(1)); // enable interrupts and start hcd

	while(_operational.load(op_regs::usbsts) & usbsts::hcHalted); // wait for start

	for (auto &p : _supportedProtocols) {
		mbus_ng::Properties descriptor{
			{"generic.devtype", mbus_ng::StringItem{"usb-controller"}},
			{"generic.devsubtype", mbus_ng::StringItem{"xhci"}},
			{"usb.version.major", mbus_ng::StringItem{std::to_string(p.major)}},
			{"usb.version.minor", mbus_ng::StringItem{std::to_string(p.minor)}},
			{"usb.root.parent", mbus_ng::StringItem{std::to_string(_entity.id())}},
		};

		auto xhciEntity = (co_await mbus_ng::Instance::global().createEntity(
					"xhci-controller", descriptor)).unwrap();

		auto hub = std::make_shared<RootHub>(this, p, std::move(xhciEntity));
		_rootHubs.push_back(hub);
		_enumerator.observeHub(hub);
	}

	printf("xhci: init done\n");
}

async::detached Controller::handleIrqs() {
	uint64_t sequence = 0;

	while(1) {
		auto await = co_await helix_ng::awaitEvent(_irq, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		if (!_interrupters[0]->isPending()) {
			HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckNack, sequence));
			continue;
		}

		_interrupters[0]->clearPending();
		HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, sequence));

		_eventRing.processRing();
		_interrupters[0]->setEventRing(&_eventRing, true);
	}

	printf("xhci: interrupt coroutine should not exit...\n");
}

async::detached Controller::handleMsis() {
	uint64_t sequence = 0;

	while(1) {
		auto await = co_await helix_ng::awaitEvent(_irq, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		// XXX: In theory, ERDP.EHB should always be set on MSI entry,
		// but if we check it, and nack if it's unset, the driver nacks
		// an IRQ from the device and essentially stalls the driver.

		HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, sequence));

		_eventRing.processRing();
		_interrupters[0]->setEventRing(&_eventRing, true);
	}

	printf("xhci: interrupt coroutine should not exit...\n");
}

void Controller::ringDoorbell(uint8_t doorbell, uint8_t target, uint16_t stream_id) {
	arch::scalar_store<uint32_t>(_doorbells, doorbell * 4,
			target | (stream_id << 16));
}

async::result<void> Controller::enumerateDevice(std::shared_ptr<proto::Hub> parentHub, int port, proto::DeviceSpeed speed) {
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
	(co_await device->enumerate(rootPort, port, route, parentHub, speed, proto->protocolSlotType)).unwrap();
	_devices[device->slot()] = device;

	// TODO: if isFullSpeed is set, read the first 8 bytes of the device descriptor
	// and update the control endpoint's max packet size to match the bMaxPacketSize0 value

	arch::dma_object<proto::DeviceDescriptor> descriptor{&_memoryPool};
	(co_await device->readDescriptor(descriptor.view_buffer(), 0x0100)).unwrap();

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
		auto hub = (co_await createHubFromDevice(parentHub, proto::Device{device}, port)).unwrap();

		(co_await device->configureHub(hub, speed)).unwrap();

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
				printf("xhci: Event for missing endpoint ID %zu on slot %u?\n", ev.endpointId, ev.slotId);
			break;

		case portStatusChangeEvent:
			assert(ev.portId <= _ports.size());
			if (_ports[ev.portId - 1])
				_ports[ev.portId - 1]->_doorbell.raise();
			break;

		default:
			printf("xhci: Unexpected event in processEvent, ignoring...\n");
			ev.printInfo();
	}
}

// ------------------------------------------------------------------------
// Controller::Interrutper
// ------------------------------------------------------------------------

Controller::Interrupter::Interrupter(int id, Controller *controller) {
	_space = controller->_runtime.subspace(0x20 + id * 32);
}

void Controller::Interrupter::setEnable(bool enable) {
	auto val = _space.load(interrupter::iman);

	if (enable) {
		val |= iman::enable(1);
	} else {
		val &= ~iman::enable;
	}

	_space.store(interrupter::iman, val);
}

void Controller::Interrupter::setEventRing(EventRing *ring, bool clearEhb) {
	// don't reload erstba if only setting erdp (indicated wanting to clear ehb)
	if (!clearEhb) {
		_space.store(interrupter::erstsz, ring->getErstSize());
		_space.store(interrupter::erstbaLow,ring->getErstPtr() & 0xFFFFFFFF);
		_space.store(interrupter::erstbaHi, ring->getErstPtr() >> 32);
	}

	_space.store(interrupter::erdpLow,
		(ring->getEventRingPtr() & 0xFFFFFFF0) | ((clearEhb ? 1 : 0) << 3));
	_space.store(interrupter::erdpHi, ring->getEventRingPtr() >> 32);
}

bool Controller::Interrupter::isPending() {
	return _space.load(interrupter::iman) & iman::pending;
}

void Controller::Interrupter::clearPending() {
	auto reg = _space.load(interrupter::iman);
	_space.store(interrupter::iman, reg | iman::pending(1));
}

// ------------------------------------------------------------------------
// Controller::Port
// ------------------------------------------------------------------------

Controller::Port::Port(int id, Controller *controller, SupportedProtocol *proto)
: _id{id}, _proto{proto} {
	_space = controller->_operational.subspace(0x400 + (id - 1) * 0x10);
}

void Controller::Port::reset() {
	printf("xhci: resetting port %d\n", _id);
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
		printf("xhci: port %u is not powered on\n", _id);
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

	printf("xhci: port link status is %u\n", linkStatus);

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

	if (ignoreControllerSpeeds || _proto->speeds.empty()) {
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
	} else {
		for (auto &pSpeed : _proto->speeds) {
			if (pSpeed.value == speedId) {
				if (pSpeed.exponent == 2
						&& pSpeed.mantissa == 12) {
					// Full Speed
					speed = proto::DeviceSpeed::fullSpeed;
				} else if (pSpeed.exponent == 1
						&& pSpeed.mantissa == 1500) {
					// Low Speed
					speed = proto::DeviceSpeed::lowSpeed;
				} else if (pSpeed.exponent == 2
						&& pSpeed.mantissa == 480) {
					// High Speed
					speed = proto::DeviceSpeed::highSpeed;
				} else if (pSpeed.exponent == 3
						&& (pSpeed.mantissa == 5
							|| pSpeed.mantissa == 10
							|| pSpeed.mantissa == 20)) {
					// SuperSpeed
					speed = proto::DeviceSpeed::superSpeed;
				}else if (pSpeed.exponent == 2
						&& (pSpeed.mantissa == 1248
							|| pSpeed.mantissa == 2496
							|| pSpeed.mantissa == 4992
							|| pSpeed.mantissa == 1458
							|| pSpeed.mantissa == 2915
							|| pSpeed.mantissa == 5830)) {
					// SSIC SuperSpeed
					speed = proto::DeviceSpeed::superSpeed;
				}

				break;
			}
		}
	}

	if (speed) {
		co_return speed.value();
	} else {
		printf("xhci: Invalid speed ID: %u\n", speedId);
		co_return proto::UsbError::unsupported;
	}
}

// ------------------------------------------------------------------------
// Controller::RootHub
// ------------------------------------------------------------------------

Controller::RootHub::RootHub(Controller *controller, SupportedProtocol &proto, mbus_ng::EntityManager entity)
: Hub{nullptr, 0}, _controller{controller}, _proto{&proto}, _entity{std::move(entity)} {
	for (size_t i = 0; i < proto.compatiblePortCount; i++) {
		_ports.push_back(std::make_unique<Port>(i + proto.compatiblePortStart, controller, &proto));
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
// Controller::Device
// ------------------------------------------------------------------------

Controller::Device::Device(Controller *controller)
: _slotId{-1}, _controller{controller} {
}

arch::dma_pool *Controller::Device::setupPool() {
	return &_controller->_memoryPool;
}

arch::dma_pool *Controller::Device::bufferPool() {
	return &_controller->_memoryPool;
}

async::result<frg::expected<proto::UsbError, std::string>>
Controller::Device::deviceDescriptor() {
	arch::dma_object<proto::DeviceDescriptor> descriptor{bufferPool()};
	FRG_CO_TRY(co_await readDescriptor(descriptor.view_buffer(), 0x0100));
	co_return std::string{(char *)descriptor.data(), descriptor.view_buffer().size()};
}

async::result<frg::expected<proto::UsbError, std::string>>
Controller::Device::configurationDescriptor(uint8_t configuration) {
	arch::dma_object<proto::ConfigDescriptor> header{&_controller->_memoryPool};
	FRG_CO_TRY(co_await readDescriptor(header.view_buffer(), 0x0200 | configuration));

	arch::dma_buffer descriptor{&_controller->_memoryPool, header->totalLength};
	FRG_CO_TRY(co_await readDescriptor(descriptor, 0x0200 | configuration));
	co_return std::string{(char *)descriptor.data(), descriptor.size()};
}

async::result<frg::expected<proto::UsbError, proto::Configuration>>
Controller::Device::useConfiguration(int number) {
	auto descriptor = FRG_CO_TRY(co_await configurationDescriptor());

	struct EndpointInfo {
		int pipe;
		proto::PipeType dir;
		int packetSize;
		proto::EndpointType type;
	};

	std::vector<EndpointInfo> _eps = {};

	proto::walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		(void)length;

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

	for (auto &ep : _eps) {
		printf("xhci: setting up %s endpoint %d (max packet size: %d)\n",
			ep.dir == proto::PipeType::in ? "in" : "out", ep.pipe, ep.packetSize);
		FRG_CO_TRY(co_await setupEndpoint(ep.pipe, ep.dir, ep.packetSize, ep.type));
	}

	arch::dma_object<proto::SetupPacket> setConfig{setupPool()};
	setConfig->type = proto::setup_type::targetDevice | proto::setup_type::byStandard | proto::setup_type::toDevice;
	setConfig->request = proto::request_type::setConfig;
	setConfig->value = number;
	setConfig->index = 0;
	setConfig->length = 0;

	FRG_CO_TRY(co_await transfer({protocols::usb::kXferToDevice, setConfig, {}}));

	printf("xhci: configuration set\n");

	co_return proto::Configuration{std::make_shared<Controller::ConfigurationState>(shared_from_this(), number)};
}

async::result<frg::expected<proto::UsbError>>
Controller::Device::transfer(proto::ControlTransfer info) {
	co_return co_await _endpoints[0]->transfer(info);
}

void Controller::Device::submit(int endpoint) {
	assert(_slotId != -1);
	_controller->ringDoorbell(_slotId, endpoint, /* stream */ 0);
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
Controller::Device::enumerate(size_t rootPort, size_t port, uint32_t route, std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed, int slotType) {
	auto event = co_await _controller->submitCommand(
			Command::enableSlot(slotType));

	FRG_CO_TRY(completionToError(event));

	assert(event.completionCode != 9); // TODO: handle running out of device slots
	assert(event.completionCode == 1); // success

	_slotId = event.slotId;

	printf("xhci: slot enabled successfully!\n");
	printf("xhci: slot id for port %lx (route %x) is %d\n", port, route, _slotId);

	// initialize slot

	_devCtx = DeviceContext{_controller->_largeCtx, &_controller->_memoryPool};

	InputContext inputCtx{_controller->_largeCtx, &_controller->_memoryPool};
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

	_controller->_dcbaa[_slotId] = helix::ptrToPhysical(_devCtx.rawData());

	event = co_await _controller->submitCommand(
			Command::addressDevice(_slotId,
				helix::ptrToPhysical(inputCtx.rawData())));

	if (event.completionCode != 1)
		printf("xhci: failed to address device, completion code: '%s'\n",
			completionCodeNames[event.completionCode]);

	FRG_CO_TRY(completionToError(event));

	printf("xhci: device successfully addressed\n");

	co_return frg::success;
}

async::result<frg::expected<proto::UsbError>>
Controller::Device::readDescriptor(arch::dma_buffer_view dest, uint16_t desc) {
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

static inline int getEndpointIndex(int endpoint, proto::PipeType dir) {
	using proto::PipeType;

	// For control endpoints the index is:
	//  DCI = (Endpoint Number * 2) + 1.
	// For interrupt, bulk, isoch, the index is:
	//  DCI = (Endpoint Number * 2) + Direction,
	//    where Direction = '0' for OUT endpoints
	//    and '1' for IN endpoints.

	return endpoint * 2 +
		((dir == PipeType::in || dir == PipeType::control)
				? 1 : 0);
}

async::result<frg::expected<proto::UsbError>>
Controller::Device::setupEndpoint(int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type) {
	InputContext inputCtx{_controller->_largeCtx, &_controller->_memoryPool};

	inputCtx.get(inputCtxCtrl) |= InputControlFields::add(0); // Slot Context
	inputCtx.get(inputCtxSlot) = _devCtx.get(deviceCtxSlot);
	inputCtx.get(inputCtxSlot) |= SlotFields::ctxEntries(31);

	_initEpCtx(inputCtx, endpoint, dir, maxPacketSize, type);

	auto event = co_await _controller->submitCommand(
			Command::configureEndpoint(_slotId,
				helix::ptrToPhysical(inputCtx.rawData())));

	if (event.completionCode != 1)
		printf("xhci: failed to configure endpoint, completion code: '%s'\n",
			completionCodeNames[event.completionCode]);

	FRG_CO_TRY(completionToError(event));

	printf("xhci: configure endpoint finished\n");

	co_return frg::success;
}

async::result<frg::expected<proto::UsbError>>
Controller::Device::configureHub(std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed) {
	InputContext inputCtx{_controller->_largeCtx, &_controller->_memoryPool};

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
		printf("xhci: failed to configure endpoint, completion code: '%s'\n",
			completionCodeNames[event.completionCode]);

	FRG_CO_TRY(completionToError(event));

	printf("xhci: configure hub finished\n");
	co_return frg::success;
}

void Controller::Device::_initEpCtx(InputContext &ctx, int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type) {
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

// ------------------------------------------------------------------------
// Controller::ConfigurationState
// ------------------------------------------------------------------------

Controller::ConfigurationState::ConfigurationState(std::shared_ptr<Device> device, int)
: _device{device} {
}

async::result<frg::expected<proto::UsbError, proto::Interface>>
Controller::ConfigurationState::useInterface(int number, int alternative) {
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
		printf("xhci: SET_INTERFACE(%d, %d) stalled, ignoring...\n", number, alternative);
	} else {
		FRG_CO_TRY(res);
	}

	co_return proto::Interface{std::make_shared<Controller::InterfaceState>(_device, number)};
}

// ------------------------------------------------------------------------
// Controller::InterfaceState
// ------------------------------------------------------------------------

Controller::InterfaceState::InterfaceState(std::shared_ptr<Device> device, int num)
: proto::InterfaceData{num}, _device{device} {
}

async::result<frg::expected<proto::UsbError, proto::Endpoint>>
Controller::InterfaceState::getEndpoint(proto::PipeType type, int number) {
	co_return proto::Endpoint{_device->endpoint(getEndpointIndex(number, type))};
}

// ------------------------------------------------------------------------
// Controller::EndpointState
// ------------------------------------------------------------------------

Controller::EndpointState::EndpointState(Device *device, int endpointId, proto::EndpointType type,
		size_t maxPacketSize)
: _device{device}, _endpointId{endpointId}, _type{type}, _maxPacketSize{maxPacketSize},
	_transferRing{device->controller()} {
}

async::result<frg::expected<proto::UsbError>>
Controller::EndpointState::transfer(proto::ControlTransfer info) {
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
			printf("xhci: Failed to reset EP %d after stall: %d!\n", _endpointId, (int)res.error());
		}
	}

	// TODO(qookie): Report the residue to the user
	(void)FRG_CO_TRY(maybeResidue);

	co_return frg::success;
}

async::result<frg::expected<proto::UsbError, size_t>>
Controller::EndpointState::_bulkOrInterruptXfer(arch::dma_buffer_view buffer) {
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
			printf("xhci: Failed to reset EP %d after stall: %d!\n", _endpointId, (int)res.error());
		}
	}

	co_return buffer.size() - FRG_CO_TRY(maybeResidue);
}

async::result<frg::expected<proto::UsbError, size_t>>
Controller::EndpointState::transfer(proto::InterruptTransfer info) {
	co_return co_await _bulkOrInterruptXfer(info.buffer);
}

async::result<frg::expected<proto::UsbError, size_t>>
Controller::EndpointState::transfer(proto::BulkTransfer info) {
	co_return co_await _bulkOrInterruptXfer(info.buffer);
}

async::result<frg::expected<proto::UsbError>>
Controller::EndpointState::_resetAfterError(size_t nextDequeue, bool cycle) {
	// Issue the Reset Endpoint command to reset the xHC state
	auto event = co_await _device->controller()->submitCommand(
		Command::resetEndpoint(_device->slot(), _endpointId));

	if (event.completionCode != 1)
		printf("xhci: failed to reset endpoint, completion code: '%s'\n",
			completionCodeNames[event.completionCode]);

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
		printf("xhci: failed to set TR dequeue ptr, completion code: '%s'\n",
			completionCodeNames[event.completionCode]);

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
		irq = co_await device.accessIrq();
	}

	co_await device.enableBusmaster();

	helix::Mapping mapping{bar, info.barInfo[0].offset, info.barInfo[0].length};

	auto controller = std::make_shared<Controller>(std::move(device), std::move(entity), std::move(mapping),
			std::move(bar), std::move(irq), info.numMsis > 0);
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
	printf("xhci: starting driver\n");

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}

