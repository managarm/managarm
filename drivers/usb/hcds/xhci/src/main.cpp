
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <deque>
#include <optional>
#include <functional>
#include <iostream>
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

// ----------------------------------------------------------------
// Controller
// ----------------------------------------------------------------

std::vector<std::shared_ptr<Controller>> globalControllers;

Controller::Controller(protocols::hw::Device hw_device, helix::Mapping mapping,
		helix::UniqueDescriptor mmio, helix::UniqueIrq irq, bool useMsis)
: _hw_device{std::move(hw_device)}, _mapping{std::move(mapping)},
		_mmio{std::move(mmio)}, _irq{std::move(irq)},
		_space{_mapping.get()}, _memoryPool{},
		_dcbaa{&_memoryPool, 256}, _cmdRing{this},
		_eventRing{this}, _useMsis{useMsis},
		_enumerator{this}, _largeCtx{false} {
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

	_operational.store(op_regs::crcr, _cmdRing.getCrcr() | 1);

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
		auto hub = std::make_shared<RootHub>(this, p);
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
	}

	printf("xhci: interrupt coroutine should not exit...\n");
}

void Controller::ringDoorbell(uint8_t doorbell, uint8_t target, uint16_t stream_id) {
	arch::scalar_store<uint32_t>(_doorbells, doorbell * 4,
			target | (stream_id << 16));
}

async::result<void> Controller::enumerateDevice(std::shared_ptr<Hub> parentHub, int port, DeviceSpeed speed) {
	uint32_t route = 0;
	size_t rootPort = port;

	if (parentHub->parent()) {
		route |= port > 14 ? 14 : (port + 1);
	}

	std::shared_ptr<Hub> h = parentHub;

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
	co_await device->enumerate(rootPort, port, route, parentHub, speed, proto->protocolSlotType);
	_devices[device->slot()] = device;

	// TODO: if isFullSpeed is set, read the first 8 bytes of the device descriptor
	// and update the control endpoint's max packet size to match the bMaxPacketSize0 value

	arch::dma_object<DeviceDescriptor> descriptor{&_memoryPool};
	co_await device->readDescriptor(descriptor.view_buffer(), 0x0100);

	// Advertise the USB device on mbus.
	char class_code[3], sub_class[3], protocol[3];
	char vendor[5], product[5], release[5];
	sprintf(class_code, "%.2x", descriptor->deviceClass);
	sprintf(sub_class, "%.2x", descriptor->deviceSubclass);
	sprintf(protocol, "%.2x", descriptor->deviceProtocol);
	sprintf(vendor, "%.4x", descriptor->idVendor);
	sprintf(product, "%.4x", descriptor->idProduct);
	sprintf(release, "%.4x", descriptor->bcdDevice);

	if (descriptor->deviceClass == 0x09 && descriptor->deviceSubclass == 0) {
		auto hub = (co_await createHubFromDevice(parentHub, ::Device{device}, port)).unwrap();

		device->configureHub(hub);

		_enumerator.observeHub(std::move(hub));
	}

	mbus::Properties mbus_desc{
		{"usb.type", mbus::StringItem{"device"}},
		{"usb.vendor", mbus::StringItem{vendor}},
		{"usb.product", mbus::StringItem{product}},
		{"usb.class", mbus::StringItem{class_code}},
		{"usb.subclass", mbus::StringItem{sub_class}},
		{"usb.protocol", mbus::StringItem{protocol}},
		{"usb.release", mbus::StringItem{release}}
	};

	auto root = co_await mbus::Instance::global().getRoot();

	char name[3];
	sprintf(name, "%.2lx", device->slot());

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		protocols::usb::serve(::Device{device}, std::move(local_lane));

		co_return std::move(remote_lane);
	});

	co_await root.createObject(name, mbus_desc, std::move(handler));
}

// ------------------------------------------------------------------------
// Controller::CommandRing
// ------------------------------------------------------------------------

Controller::CommandRing::CommandRing(Controller *controller)
:_commandRing{&controller->_memoryPool}, _enqueuePtr{0},
	_controller{controller}, _pcs{true} {

	for (uint32_t i = 0; i < commandRingSize; i++) {
		_commandRing->ent[i] = {{0, 0, 0, 0}};
	}

	_commandRing->ent[commandRingSize - 1] = {{
		static_cast<uint32_t>(getCrcr() & 0xFFFFFFFF),
		static_cast<uint32_t>(getCrcr() >> 32),
		0,
		static_cast<uint32_t>(_pcs | (1 << 1) | (1 << 5) | (6 << 10))
	}};
}

uintptr_t Controller::CommandRing::getCrcr() {
	uintptr_t ptr;
	HEL_CHECK(helPointerPhysical(_commandRing.data(), &ptr));
	return ptr;
}

void Controller::CommandRing::pushRawCommand(RawTrb cmd, 
		Controller::CommandRing::CommandEvent *ev) {
	assert(_enqueuePtr < 127 && "ring aspect of the command ring not yet supported");
	_commandRing->ent[_enqueuePtr] = cmd;
	_commandEvents[_enqueuePtr] = ev;
	if (_pcs) {
		_commandRing->ent[_enqueuePtr].val[3] |= 1;
	} else {
		_commandRing->ent[_enqueuePtr].val[3] &= ~1;
	}
	_enqueuePtr++;

	// update link trb
	_commandRing->ent[commandRingSize - 1] = {{
		static_cast<uint32_t>(getCrcr() & 0xFFFFFFFF),
		static_cast<uint32_t>(getCrcr() >> 32),
		0,
		static_cast<uint32_t>(_pcs | (1 << 1) | (1 << 5) | (6 << 10))
	}};
}

void Controller::CommandRing::submit() {
	_controller->ringDoorbell(0, 0, 0);
}

// ------------------------------------------------------------------------
// Controller::EventRing
// ------------------------------------------------------------------------

Controller::EventRing::EventRing(Controller *controller)
:_eventRing{&controller->_memoryPool}, _erst{&controller->_memoryPool, 1},
	_dequeuePtr{0}, _controller{controller}, _ccs{1} {

	for (size_t i = 0; i < eventRingSize; i++) {
		_eventRing->ent[i] = {{0, 0, 0, 0}};
	}

	_erst[0].ringSegmentBaseLow = getEventRingPtr() & 0xFFFFFFFF;
	_erst[0].ringSegmentBaseHi = getEventRingPtr() >> 32;
	_erst[0].ringSegmentSize = eventRingSize;
	_erst[0].reserved = 0; // ResvZ in spec
}

uintptr_t Controller::EventRing::getErstPtr() {
	uintptr_t ptr;
	HEL_CHECK(helPointerPhysical(_erst.data(), &ptr));
	return ptr;
}

uintptr_t Controller::EventRing::getEventRingPtr() {
	uintptr_t ptr;
	HEL_CHECK(helPointerPhysical(_eventRing.data(), &ptr));
	return ptr + _dequeuePtr * sizeof(RawTrb);
}

size_t Controller::EventRing::getErstSize() {
	return _erst.size();
}

void Controller::EventRing::processRing() {
	while((_eventRing->ent[_dequeuePtr].val[3] & 1) == _ccs) {
		RawTrb raw_ev = _eventRing->ent[_dequeuePtr];

		int old_ccs = _ccs;

		_dequeuePtr++;
		if (_dequeuePtr >= eventRingSize) {
			_dequeuePtr = 0; // wrap around
			_ccs = !_ccs; // invert cycle state
		}

		if ((raw_ev.val[3] & 1) != old_ccs)
			break; // not the proper cycle state

		Controller::Event ev = Controller::Event::fromRawTrb(raw_ev);

		_dequeuedEvents.push_back(ev);
		processEvent(ev);
	}

	_controller->_interrupters[0]->setEventRing(this, true);
	_doorbell.raise();
}

void Controller::EventRing::processEvent(Controller::Event ev) {
	if (ev.type == TrbType::commandCompletionEvent) {
		size_t commandIndex = (ev.commandPointer - _controller->_cmdRing.getCrcr()) / sizeof(RawTrb);
		assert(commandIndex < Controller::CommandRing::commandRingSize);
		auto cmdEv = _controller->_cmdRing._commandEvents[commandIndex];
		_controller->_cmdRing._commandEvents[commandIndex] = nullptr;
		if (cmdEv) {
			cmdEv->event = ev;
			cmdEv->completion.raise();
		}
	} else if (ev.type == TrbType::portStatusChangeEvent) {
		printf("xhci: port %lu changed state\n", ev.portId);
		assert(ev.portId <= _controller->_ports.size());
		if (_controller->_ports[ev.portId - 1])
			_controller->_ports[ev.portId - 1]->_doorbell.raise();
	} else if (ev.type == TrbType::transferEvent) {
		auto transferRing = _controller->_devices[ev.slotId]->_transferRings[ev.endpointId - 1].get();
		size_t commandIndex = (ev.trbPointer - transferRing->getPtr()) / sizeof(RawTrb);
		assert(commandIndex < Controller::TransferRing::transferRingSize);
		auto transferEv = transferRing->_transferEvents[commandIndex];
		transferRing->_transferEvents[commandIndex] = nullptr;
		if (transferEv) {
			transferEv->event = ev;
			transferEv->completion.raise();
		}
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
// Controller::Event
// ------------------------------------------------------------------------

Controller::Event Controller::Event::fromRawTrb(RawTrb trb) {
	Controller::Event ev;

	ev.type = static_cast<TrbType>((trb.val[3] >> 10) & 63);
	ev.completionCode = (trb.val[2] >> 24) & 0xFF;
	ev.slotId = (trb.val[3] >> 24) & 0xFF;
	ev.vfId = (trb.val[3] >> 16) & 0xFF;
	ev.raw = trb;

	switch(ev.type) {
		case TrbType::transferEvent:
			ev.trbPointer = trb.val[0] |
				(static_cast<uintptr_t>(trb.val[1]) << 32);
			ev.transferLen = trb.val[2] & 0xFFFFFF;
			ev.endpointId = (trb.val[3] >> 16) & 0x1F;
			ev.eventData = trb.val[3] & (1 << 2);
			break;

		case TrbType::commandCompletionEvent:
			ev.commandPointer = trb.val[0] |
				(static_cast<uintptr_t>(trb.val[1]) << 32);
			ev.commandCompletionParameter = trb.val[2] & 0xFFFFFF;
			break;

		case TrbType::portStatusChangeEvent:
			ev.portId = (trb.val[0] >> 24) & 0xFF;
			break;

		case TrbType::doorbellEvent:
			ev.doorbellReason = trb.val[0] & 0x1F;
			break;

		case TrbType::deviceNotificationEvent:
			ev.notificationData = (trb.val[0] |
				(static_cast<uintptr_t>(trb.val[1]) << 32))
				>> 8;
			ev.notificationType = (trb.val[0] >> 4) & 0xF;
			break;

		default:
			assert(!"xhci: trb passed to fromRawTrb is not a proper event trb\n");
	}

	return ev;
}

void Controller::Event::printInfo() {
	printf("xhci: --- event dump ---\n");
	printf("xhci: raw: %08x %08x %08x %08x\n",
			raw.val[0], raw.val[1], raw.val[2], raw.val[3]);
	printf("xhci: type: %u\n", static_cast<unsigned int>(type));
	printf("xhci: slot id: %d\n", slotId);
	printf("xhci: completion code: %s (%d)\n",
			completionCodeNames[completionCode],
			completionCode);

	switch(type) {
		case TrbType::transferEvent:
			printf("xhci: type name: Transfer Event\n");
			printf("xhci: trb ptr: %016lx, len %lu\n", trbPointer,
					transferLen);
			printf("xhci: endpointId: %lu, eventData: %s\n",
					endpointId, eventData ? "yes" : "no");
			break;
		case TrbType::commandCompletionEvent:
			printf("xhci: type name: Command Completion Event\n");
			printf("xhci: command pointer: %016lx\n",
					commandPointer);
			printf("xhci: command completion parameter: %d\n",
					commandCompletionParameter);
			printf("xhci: vfid: %d\n", vfId);
			break;
		case TrbType::portStatusChangeEvent:
			printf("xhci: type name: Port Status Change Event\n");
			printf("xhci: port id: %lu\n", portId);
			break;
		case TrbType::bandwidthRequestEvent:
			printf("xhci: type name: Bandwidth Request Event\n");
			break;
		case TrbType::doorbellEvent:
			printf("xhci: type name: Doorbell Event\n");
			printf("xhci: reason: %lu\n", doorbellReason);
			printf("xhci: vfid: %d\n", vfId);
			break;
		case TrbType::hostControllerEvent:
			printf("xhci: type name: Host Controller Event\n");
			break;
		case TrbType::deviceNotificationEvent:
			printf("xhci: type name: Device Notification Event\n");
			printf("xhci: notification data: %lx\n",
					notificationData);
			printf("xhci: notification type: %lu\n",
					notificationType);
			break;
		case TrbType::mfindexWrapEvent:
			printf("xhci: type name: MFINDEX Wrap Event\n");
			break;
		default:
			printf("xhci: invalid event\n");
	}

	printf("xhci: --- end of event dump ---\n");
}

// ------------------------------------------------------------------------
// Controller::Port
// ------------------------------------------------------------------------

Controller::Port::Port(int id, Controller *controller, SupportedProtocol *proto)
: _id{id}, _controller{controller}, _proto{proto} {
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

	if (_proto->major == 2) {
		// await CCS=1
		co_await awaitFlag(portsc::connectStatus, true);

		// The XHCI spec states that USB2 devices should enter the polling state at the
		// same time they set CCS=1, but VirtualBox' XHCI does not implement this behavior.
		auto linkStatus = getLinkStatus();
		if(linkStatus != 7)
			printf("\e[35m" "xhci: USB2 port did not enter polling state after CCS=1" "\e[39m\n");

		reset();

		// await PED=1
		co_await awaitFlag(portsc::portEnable, true);
	} else if (_proto->major == 3) {
		// XXX: is this enough?
		// await PED=1
		co_await awaitFlag(portsc::portEnable, true);
	}

	auto linkStatus = getLinkStatus();

	printf("xhci: port link status is %u\n", linkStatus);

	if (linkStatus >= 1 && linkStatus <= 3) {
		transitionToLinkStatus(0);
	} else {
		assert(linkStatus == 0); // U0
	}

	_state.changes |= HubStatus::connect | HubStatus::enable;
	_state.status |= HubStatus::connect | HubStatus::enable;
	_pollEv.raise();
}

async::result<PortState> Controller::Port::pollState() {
	_pollSeq = co_await _pollEv.async_wait(_pollSeq);
	co_return _state;
}

async::result<frg::expected<UsbError, DeviceSpeed>> Controller::Port::getGenericSpeed() {
	uint8_t speedId = getSpeed();

	std::optional<DeviceSpeed> speed;

	for (auto &pSpeed : _proto->speeds) {
		if (pSpeed.value == speedId) {
			if (pSpeed.exponent == 2
					&& pSpeed.mantissa == 12) {
				// Full Speed
				speed = DeviceSpeed::fullSpeed;
			} else if (pSpeed.exponent == 1
					&& pSpeed.mantissa == 1500) {
				// Low Speed
				speed = DeviceSpeed::lowSpeed;
			} else if (pSpeed.exponent == 2
					&& pSpeed.mantissa == 480) {
				// High Speed
				speed = DeviceSpeed::highSpeed;
			} else if (pSpeed.exponent == 3
					&& (pSpeed.mantissa == 5
						|| pSpeed.mantissa == 10
						|| pSpeed.mantissa == 20)) {
				// SuperSpeed
				speed = DeviceSpeed::superSpeed;
			}else if (pSpeed.exponent == 2
					&& (pSpeed.mantissa == 1248
						|| pSpeed.mantissa == 2496
						|| pSpeed.mantissa == 4992
						|| pSpeed.mantissa == 1458
						|| pSpeed.mantissa == 2915
						|| pSpeed.mantissa == 5830)) {
				// SSIC SuperSpeed
				speed = DeviceSpeed::superSpeed;
			}

			break;
		}
	}

	if (_proto->speeds.empty()) {
		switch(speedId) {
			case 1:
				speed = DeviceSpeed::fullSpeed;
				break;
			case 2:
				speed = DeviceSpeed::lowSpeed;
				break;
			case 3:
				speed = DeviceSpeed::highSpeed;
				break;
			case 4:
			case 5:
			case 6:
			case 7:
				speed = DeviceSpeed::superSpeed;
		}
	}

	// Raise the event here to make progress in the enumerator
	_pollEv.raise();

	assert(speed);

	if (speed)
		co_return speed.value();
	else
		co_return UsbError::stall; // TODO(qookie): Add a better error
}

// ------------------------------------------------------------------------
// Controller::RootHub
// ------------------------------------------------------------------------

Controller::RootHub::RootHub(Controller *controller, SupportedProtocol &proto)
: Hub{nullptr, 0}, _controller{controller}, _proto{&proto} {
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

async::result<PortState> Controller::RootHub::pollState(int port) {
	co_return co_await _ports[port]->pollState();
}

async::result<frg::expected<UsbError, DeviceSpeed>> Controller::RootHub::issueReset(int port) {
	co_return FRG_CO_TRY(co_await _ports[port]->getGenericSpeed());
}

// ------------------------------------------------------------------------
// Controller::TransferRing
// ------------------------------------------------------------------------

Controller::TransferRing::TransferRing(Controller *controller)
: _transferRing{&controller->_memoryPool}, _enqueuePtr{0}, _pcs{true} {

	for (uint32_t i = 0; i < transferRingSize; i++) {
		_transferRing->ent[i] = {{0, 0, 0, 0}};
	}

	_transferRing->ent[transferRingSize - 1] = {{
		static_cast<uint32_t>(getPtr() & 0xFFFFFFFF),
		static_cast<uint32_t>(getPtr() >> 32),
		0,
		static_cast<uint32_t>(_pcs | (1 << 1) | (1 << 5) | (6 << 10))
	}};
}

uintptr_t Controller::TransferRing::getPtr() {
	uintptr_t ptr;
	HEL_CHECK(helPointerPhysical(_transferRing.data(), &ptr));
	return ptr;
}

void Controller::TransferRing::pushRawTransfer(RawTrb cmd, 
		Controller::TransferRing::TransferEvent *ev) {

	_transferRing->ent[_enqueuePtr] = cmd;
	_transferEvents[_enqueuePtr] = ev;
	if (_pcs) {
		_transferRing->ent[_enqueuePtr].val[3] |= 1;
	} else {
		_transferRing->ent[_enqueuePtr].val[3] &= ~1;
	}
	_enqueuePtr++;

	if (_enqueuePtr >= Controller::TransferRing::transferRingSize - 1) {
		updateLink();
		_pcs = !_pcs;
		_enqueuePtr = 0;
	}
}

void Controller::TransferRing::updateLink() {
	_transferRing->ent[transferRingSize - 1] = {{
		static_cast<uint32_t>(getPtr() & 0xFFFFFFFF),
		static_cast<uint32_t>(getPtr() >> 32),
		0,
		static_cast<uint32_t>(_pcs | (1 << 1) | (1 << 5) | (6 << 10))
	}};
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

async::result<frg::expected<UsbError, std::string>>
Controller::Device::configurationDescriptor() {
	arch::dma_object<ConfigDescriptor> header{&_controller->_memoryPool};
	co_await readDescriptor(header.view_buffer(), 0x0200);

	arch::dma_buffer descriptor{&_controller->_memoryPool, header->totalLength};
	co_await readDescriptor(descriptor, 0x0200);
	co_return std::string{(char *)descriptor.data(), descriptor.size()};
}

async::result<frg::expected<UsbError, Configuration>>
Controller::Device::useConfiguration(int number) {
	auto descriptor = FRG_CO_TRY(co_await configurationDescriptor());

	struct EndpointInfo {
		int pipe;
		PipeType dir;
		int packetSize;
		EndpointType type;
	};

	std::vector<EndpointInfo> _eps = {};

	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		(void)length;

		if(type != descriptor_type::endpoint)
			return;
		auto desc = (EndpointDescriptor *)p;

		// TODO: Pay attention to interface/alternative.
		auto packetSize = desc->maxPacketSize & 0x7FF;
		auto epType = info.endpointType.value();

		int pipe = info.endpointNumber.value();
		if (info.endpointIn.value()) {
			_eps.push_back({pipe, PipeType::in, packetSize, epType});
		} else {
			_eps.push_back({pipe, PipeType::out, packetSize, epType});
		}
	});

	for (auto &ep : _eps) {
		printf("xhci: setting up %s endpoint %d (max packet size: %d)\n", 
			ep.dir == PipeType::in ? "in" : "out", ep.pipe, ep.packetSize);
		co_await setupEndpoint(ep.pipe, ep.dir, ep.packetSize, ep.type);
	}

	RawTrb setup_stage = {{
			static_cast<uint32_t>((number << 16) | (9 << 8) | 0x00), // SET_CONFIGURATION, host to device
			0, 8,
			(3 << 16) | (1 << 6) | (static_cast<uint32_t>(TrbType::setupStage) << 10)}};

	RawTrb status_stage = {{
			0, 0, 0, 
			(1 << 5) | (static_cast<uint32_t>(TrbType::statusStage) << 10)}};

	TransferRing::TransferEvent ev;

	pushRawTransfer(0, setup_stage);
	pushRawTransfer(0, status_stage, &ev);
	submit(1);

	co_await ev.completion.wait();

	if (ev.event.completionCode != 1)
		printf("xhci: failed to use configuration, completion code: '%s'\n",
			completionCodeNames[ev.event.completionCode]);

	printf("xhci: configuration set\n");

	co_return Configuration{std::make_shared<Controller::ConfigurationState>(_controller, shared_from_this(), number)};
}

async::result<frg::expected<UsbError>>
Controller::Device::transfer(ControlTransfer info) {
	RawTrb setup_stage = {{
		0, static_cast<uint32_t>(info.buffer.size() << 16), 8,
		((info.flags == kXferToDevice ? 2 : 3) << 16) 
		| (1 << 6) | (static_cast<uint32_t>(TrbType::setupStage) << 10)}};

	memcpy(setup_stage.val, info.setup.data(), sizeof(SetupPacket));

	pushRawTransfer(0, setup_stage);

	size_t progress = 0;
	while(progress < info.buffer.size()) {
		uintptr_t ptr = (uintptr_t)info.buffer.data() + progress;
		uintptr_t pptr = helix::addressToPhysical(ptr);

		auto chunk = std::min(info.buffer.size() - progress, 0x1000 - (ptr & 0xFFF));

		bool is_last = (progress + chunk) >= info.buffer.size();

		RawTrb transfer = {{
			static_cast<uint32_t>(pptr & 0xFFFFFFFF),
			static_cast<uint32_t>(pptr >> 32),
			static_cast<uint32_t>(chunk),
			(!is_last << 4) | (1 << 2)
				| ((info.flags == kXferToDevice ? 0 : 1) << 16)
				| (static_cast<uint32_t>(TrbType::normal) << 10)}};

		pushRawTransfer(0, transfer);

		progress += chunk;
	}

	TransferRing::TransferEvent ev;

	RawTrb status_stage = {{
			0, 0, 0, 
			((info.flags == kXferToDevice ? 0 : 1) << 16) 
			| (1 << 5) | (static_cast<uint32_t>(TrbType::statusStage) << 10)}};

	pushRawTransfer(0, status_stage, &ev);
	submit(1);

	co_await ev.completion.wait();

	if (ev.event.completionCode != 1)
		printf("xhci: failed to perform a control transfer, completion code: '%s'\n",
			completionCodeNames[ev.event.completionCode]);
	co_return {};
}

void Controller::Device::submit(int endpoint) {
	assert(_slotId != -1);
	_controller->ringDoorbell(_slotId, endpoint, /* stream */ 0);
}

static inline uint8_t getHcdSpeedId(DeviceSpeed speed) {
	switch (speed) {
		using enum DeviceSpeed;

		case lowSpeed: return 2;
		case fullSpeed: return 1;
		case highSpeed: return 3;
		case superSpeed: return 4;
	}

	return 0;
}

async::result<void> Controller::Device::enumerate(size_t rootPort, size_t port, uint32_t route, std::shared_ptr<Hub> hub, DeviceSpeed speed, int slotType) {
	auto event = co_await _controller->submitCommand(
			Command::enableSlot(slotType));

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

	if ((speed == DeviceSpeed::lowSpeed || speed == DeviceSpeed::fullSpeed)
			&& hub->parent()) {
		// We need to fill these fields out for split transactions.

		auto hubDevice = std::static_pointer_cast<Device>(hub->associatedDevice()->state());

		slotCtx |= SlotFields::parentHubPort(hub->port() + 1);
		slotCtx |= SlotFields::parentHubSlot(hubDevice->_slotId);
	}

	slotCtx |= SlotFields::rootHubPort(rootPort);

	size_t packetSize = 0;
	switch (speed) {
		using enum DeviceSpeed;

		case lowSpeed:
		case fullSpeed: packetSize = 8; break;
		case highSpeed: packetSize = 64; break;
		case superSpeed: packetSize = 512; break;
	}

	_initEpCtx(inputCtx, 0, PipeType::control, packetSize, EndpointType::control);

	_controller->_dcbaa[_slotId] = helix::ptrToPhysical(_devCtx.rawData());

	event = co_await _controller->submitCommand(
			Command::addressDevice(_slotId,
				helix::ptrToPhysical(inputCtx.rawData())));

	if (event.completionCode != 1)
		printf("xhci: failed to address device, completion code: '%s'\n",
			completionCodeNames[event.completionCode]);

	printf("xhci: device successfully addressed\n");
}

void Controller::Device::pushRawTransfer(int endpoint, RawTrb cmd, Controller::TransferRing::TransferEvent *ev) {
	_transferRings[endpoint]->pushRawTransfer(cmd, ev);
}

async::result<void> Controller::Device::readDescriptor(arch::dma_buffer_view dest, uint16_t desc) {
	RawTrb setup_stage = {{
			static_cast<uint32_t>((desc << 16) | (6 << 8) | 0x80), // GET_DESCRIPTOR, dev to host
			static_cast<uint32_t>(dest.size() << 16), 8,
			(3 << 16) | (1 << 6) | (static_cast<uint32_t>(TrbType::setupStage) << 10)}};

	uintptr_t ptr = helix::ptrToPhysical(dest.data());

	RawTrb data_stage = {{
			static_cast<uint32_t>(ptr & 0xFFFFFFFF),
			static_cast<uint32_t>(ptr >> 32),
			static_cast<uint32_t>(dest.size()),
			(1 << 2) | (1 << 16) | (static_cast<uint32_t>(TrbType::dataStage) << 10)}};

	RawTrb status_stage = {{
			0, 0, 0, 
			(1 << 5) | (static_cast<uint32_t>(TrbType::statusStage) << 10)}};

	TransferRing::TransferEvent ev;

	pushRawTransfer(0, setup_stage);
	pushRawTransfer(0, data_stage);
	pushRawTransfer(0, status_stage, &ev);
	submit(1);

	co_await ev.completion.wait();

	if (ev.event.completionCode != 1)
		printf("xhci: failed to read descriptor, completion code: '%s'\n",
			completionCodeNames[ev.event.completionCode]);

	printf("xhci: device descriptor successfully read\n");
}

static inline uint32_t getHcdEndpointType(PipeType dir, EndpointType type) {
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

static inline uint32_t getDefaultAverageTrbLen(EndpointType type) {
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

async::result<void> Controller::Device::setupEndpoint(int endpoint, PipeType dir, size_t maxPacketSize, EndpointType type, bool drop) {
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

	assert(event.completionCode == 1);

	printf("xhci: configure endpoint finished\n");
}

async::result<void> Controller::Device::configureHub(std::shared_ptr<Hub> hub) {
	InputContext inputCtx{_controller->_largeCtx, &_controller->_memoryPool};

	inputCtx.get(inputCtxCtrl) |= InputControlFields::add(0); // Slot Context
	inputCtx.get(inputCtxSlot) = _devCtx.get(deviceCtxSlot);

	inputCtx.get(inputCtxSlot) |= SlotFields::hub(true);
	inputCtx.get(inputCtxSlot) |= SlotFields::portCount(hub->numPorts());

	// TODO(qookie): Check if this device is high-speed.
	inputCtx.get(inputCtxSlot) |= SlotFields::ttThinkTime(
			hub->getCharacteristics().unwrap().ttThinkTime / 8 - 1);

	printf("xhci: ttThinkTime: %d\n", hub->getCharacteristics().unwrap().ttThinkTime);

	auto event = co_await _controller->submitCommand(
			Command::evaluateContext(_slotId,
				helix::ptrToPhysical(inputCtx.rawData())));

	if (event.completionCode != 1)
		printf("xhci: failed to configure endpoint, completion code: '%s'\n",
			completionCodeNames[event.completionCode]);

	assert(event.completionCode == 1);

	printf("xhci: configure endpoint finished\n");
}

void Controller::Device::_initEpCtx(InputContext &ctx, int endpoint, PipeType dir, size_t maxPacketSize, EndpointType type) {
	int endpointId = endpoint * 2
			+ ((dir == PipeType::in || dir == PipeType::control)
				? 1
				: 0);

	ctx.get(inputCtxCtrl) |= InputControlFields::add(endpointId); // EP Context

	_transferRings[endpointId - 1] = std::make_unique<ProducerRing>(_controller);

	auto trPtr = _transferRings[endpointId - 1]->getPtr();

	auto &epCtx = ctx.get(inputCtxEp0 + endpointId - 1);

	epCtx |= EpFields::errorCount(3);
	epCtx |= EpFields::epType(getHcdEndpointType(dir, type));
	epCtx |= EpFields::maxPacketSize(maxPacketSize);
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

Controller::ConfigurationState::ConfigurationState(Controller *controller, 
		std::shared_ptr<Device> device, int)
:_controller{controller}, _device{device} {
}

async::result<frg::expected<UsbError, Interface>>
Controller::ConfigurationState::useInterface(int number, int alternative) {
	assert(!alternative);
	co_return Interface{std::make_shared<Controller::InterfaceState>(_controller, _device, number)};
}

// ------------------------------------------------------------------------
// Controller::InterfaceState
// ------------------------------------------------------------------------

Controller::InterfaceState::InterfaceState(Controller *controller, 
		std::shared_ptr<Device> device, int)
: _controller{controller}, _device{device} {
}

async::result<frg::expected<UsbError, Endpoint>>
Controller::InterfaceState::getEndpoint(PipeType type, int number) {
	co_return Endpoint{std::make_shared<Controller::EndpointState>(_controller, _device, number, type)};
}

// ------------------------------------------------------------------------
// Controller::EndpointState
// ------------------------------------------------------------------------

Controller::EndpointState::EndpointState(Controller *, 
		std::shared_ptr<Device> device, int endpoint, PipeType type)
: _device{device}, _endpoint{endpoint}, _type{type} {
}

async::result<frg::expected<UsbError>>
Controller::EndpointState::transfer(ControlTransfer info) {
	assert(!"TODO: implement this");
	co_return {};
}

async::result<frg::expected<UsbError, size_t>>
Controller::EndpointState::transfer(InterruptTransfer info) {
	int endpointId = _endpoint * 2 + (_type == PipeType::in ? 1 : 0);

	Controller::TransferRing::TransferEvent ev;

	size_t progress = 0;
	while(progress < info.buffer.size()) {
		uintptr_t ptr = (uintptr_t)info.buffer.data() + progress;
		uintptr_t pptr = helix::addressToPhysical(ptr);

		auto chunk = std::min(info.buffer.size() - progress, 0x1000 - (ptr & 0xFFF));

		bool is_last = (progress + chunk) >= info.buffer.size();

		RawTrb transfer = {{
			static_cast<uint32_t>(pptr & 0xFFFFFFFF),
			static_cast<uint32_t>(pptr >> 32),
			static_cast<uint32_t>(chunk),
			(!is_last << 4) | (1 << 2) | (is_last << 5)
				| (static_cast<uint32_t>(TrbType::normal) << 10)}};

		_device->pushRawTransfer(endpointId - 1, transfer, is_last ? &ev : nullptr);

		progress += chunk;
	}

	_device->submit(endpointId);

	co_await ev.completion.wait();

	assert(ev.event.completionCode == 1 || ev.event.completionCode == 13); // success

	co_return info.buffer.size() - ev.event.transferLen;
}

async::result<frg::expected<UsbError, size_t>>
Controller::EndpointState::transfer(BulkTransfer info) {
	int endpointId = _endpoint * 2 + (_type == PipeType::in ? 1 : 0);

	Controller::TransferRing::TransferEvent ev;

	size_t progress = 0;
	while(progress < info.buffer.size()) {
		uintptr_t ptr = (uintptr_t)info.buffer.data() + progress;
		uintptr_t pptr = helix::addressToPhysical(ptr);

		auto chunk = std::min(info.buffer.size() - progress, 0x1000 - (ptr & 0xFFF));

		bool is_last = (progress + chunk) >= info.buffer.size();

		RawTrb transfer = {{
			static_cast<uint32_t>(pptr & 0xFFFFFFFF),
			static_cast<uint32_t>(pptr >> 32),
			static_cast<uint32_t>(chunk),
			(!is_last << 4) | (1 << 2) | (is_last << 5)
				| (static_cast<uint32_t>(TrbType::normal) << 10)}};

		_device->pushRawTransfer(endpointId - 1, transfer, is_last ? &ev : nullptr);

		progress += chunk;
	}

	_device->submit(endpointId);

	co_await ev.completion.wait();

	if (ev.event.completionCode != 1) {
		printf("xhci: completion code is %s instead of success\n", completionCodeNames[ev.event.completionCode]);
	}

	assert(ev.event.completionCode == 1); // success

	co_return info.buffer.size() - ev.event.transferLen;
}

// ------------------------------------------------------------------------
// Freestanding PCI discovery functions.
// ------------------------------------------------------------------------

async::detached bindController(mbus::Entity entity) {
	protocols::hw::Device device(co_await entity.bind());
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

	auto controller = std::make_shared<Controller>(std::move(device), std::move(mapping),
			std::move(bar), std::move(irq), info.numMsis > 0);
	controller->initialize();
	globalControllers.push_back(std::move(controller));
}

async::detached observeControllers() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-class", "0c"),
		mbus::EqualsFilter("pci-subclass", "03"),
		mbus::EqualsFilter("pci-interface", "30")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		printf("xhci: detected controller\n");
		bindController(std::move(entity));
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("xhci: starting driver\n");

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}
 
