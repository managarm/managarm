
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
#include <protocols/usb/server.hpp>

#include <helix/memory.hpp>

#include "spec.hpp"
#include "xhci.hpp"

constexpr const char *completionCodeNames[256] = {
	"Invalid",
	"Success",
	"Data buffer error",
	"Babble detected",
	"USB transaction error",
	"TRB error",
	"Stall error",
	"Resource error",
	"Bandwidth error",
	"No slots available",
	"Invalid stream type",
	"Slot not enabled",
	"Endpoint not enabled",
	"Short packet",
	"Ring underrun",
	"Ring overrun",
	"VF event ring full",
	"Parameter error",
	"Bandwidth overrun",
	"Context state error",
	"No ping response",
	"Event ring full",
	"Incompatible device",
	"Missed service",
	"Command ring stopped",
	"Command aborted",
	"Stopped",
	"Stopped - invalid length",
	"Stopped - short packet",
	"Max exit latency too high",
	"Reserved",
	"Isoch buffer overrun",
	"Event lost",
	"Undefined error",
	"Invalid stream ID",
	"Secondary bandwidth error",
	"Split transaction error",
};

// ----------------------------------------------------------------
// Controller
// ----------------------------------------------------------------

std::vector<std::shared_ptr<Controller>> globalControllers;

Controller::Controller(protocols::hw::Device hw_device, helix::Mapping mapping,
		helix::UniqueDescriptor mmio, helix::UniqueIrq irq)
: _hw_device{std::move(hw_device)}, _mapping{std::move(mapping)},
		_mmio{std::move(mmio)}, _irq{std::move(irq)},
		_space{_mapping.get()}, _memoryPool{},
		_dcbaa{&_memoryPool, 256}, _cmdRing{this},
		_eventRing{this} { 
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

	assert(!(_space.load(cap_regs::hccparams1) & hccparams1::contextSize) && "device has 64-byte contexts, which are unsupported");

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

		uintptr_t phys;
		HEL_CHECK(helPointerPhysical(_scratchpadBufs.back().data(), &phys));
		_scratchpadBufArray[i] = phys;
	}

	for (size_t i = 0; i < 256; i++)
		_dcbaa[i] = 0;

	uintptr_t sbufs_phys;
	HEL_CHECK(helPointerPhysical(_scratchpadBufArray.data(), &sbufs_phys));
	_dcbaa[0] = sbufs_phys;

	uintptr_t dcbaap;
	HEL_CHECK(helPointerPhysical(_dcbaa.data(), &dcbaap));
	_operational.store(op_regs::dcbaap, dcbaap); // tell the device about our dcbaa

	_operational.store(op_regs::crcr, _cmdRing.getCrcr() | 1);

	printf("xhci: setting up interrupters\n");
	auto max_intrs = _space.load(cap_regs::hcsparams1) & hcsparams1::maxIntrs;
	printf("xhci: max interrupters: %u\n", max_intrs);

	_interrupters.push_back(std::make_unique<Interrupter>(0, this));

	for (int i = 1; i < max_intrs; i++)
		_interrupters.push_back(std::make_unique<Interrupter>(i, this));

	_interrupters[0]->setEventRing(&_eventRing);
	_interrupters[0]->setEnable(true);

	co_await _hw_device.enableBusIrq();
	handleIrqs();

	_ports.resize(_numPorts);

	for (auto &p : _supportedProtocols) {
		for (size_t i = p.compatiblePortStart; i < (p.compatiblePortStart + p.compatiblePortCount); i++) {
			_ports[i - 1] = std::make_unique<Port>(i, this, &p);
			_ports[i - 1]->initPort();
		}
	}

	_operational.store(op_regs::usbcmd, usbcmd::run(1) | usbcmd::intrEnable(1)); // enable interrupts and start hcd

	while(_operational.load(op_regs::usbsts) & usbsts::hcHalted); // wait for start

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

void Controller::ringDoorbell(uint8_t doorbell, uint8_t target, uint16_t stream_id) {
	arch::scalar_store<uint32_t>(_doorbells, doorbell * 4,
			target | (stream_id << 16));
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
			cmdEv->promise.set_value();
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
			transferEv->promise.set_value();
		}

		transferRing->updateDequeue(commandIndex);
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

		auto linkStatus = getLinkStatus();
		assert(linkStatus == 7); // Polling

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
	} else
		assert(linkStatus == 0); // U0

	int targetPacketSize = -1;
	bool isFullSpeed = false;
	uint8_t speedId = getSpeed();

	for (auto &speed : _proto->speeds) {
		if (speed.value == speedId) {
			if (speed.exponent == 2
				&& speed.mantissa == 12) { // Full Speed
				isFullSpeed = true;
				targetPacketSize = 8;
			} else if (speed.exponent == 1
				&& speed.mantissa == 1500) { // Low Speed
				targetPacketSize = 8;
			} else if (speed.exponent == 2
				&& speed.mantissa == 480) { // High Speed
				targetPacketSize = 64;
			} else if (speed.exponent == 3
				&& (speed.mantissa == 5
					|| speed.mantissa == 10
					|| speed.mantissa == 20)) { // SuperSpeed
				targetPacketSize = 512;
			}

			break;
		}
	}

	if (_proto->speeds.empty()) {
		switch(speedId) {
			case 1:
				isFullSpeed = true;
			case 2:
				targetPacketSize = 8;
				break;
			case 3:
				targetPacketSize = 64;
				break;
			case 4:
			case 5:
			case 6:
			case 7:
				targetPacketSize = 512;
		}
	}

	assert(targetPacketSize != -1);

	_device = std::make_shared<Device>(_id, _controller);
	co_await _device->allocSlot(_proto->protocolSlotType, targetPacketSize);
	_controller->_devices[_device->_slotId] = _device;

	// TODO: if isFullSpeed is set, read the first 8 bytes of the device descriptor
	// and update the control endpoint's max packet size to match the bMaxPacketSize0 value

	arch::dma_object<DeviceDescriptor> descriptor{&_controller->_memoryPool};
	co_await _device->readDescriptor(descriptor.view_buffer(), 0x0100);

	// Advertise the USB device on mbus.
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

	auto root = co_await mbus::Instance::global().getRoot();

	char name[3];
	sprintf(name, "%.2x", _id);

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		protocols::usb::serve(::Device{_device}, std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});

	co_await root.createObject(name, mbus_desc, std::move(handler));
}

// ------------------------------------------------------------------------
// Controller::TransferRing
// ------------------------------------------------------------------------

Controller::TransferRing::TransferRing(Controller *controller)
:_transferRing{&controller->_memoryPool}, _dequeuePtr{0}, _enqueuePtr{0},
	_pcs{true} {

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

void Controller::TransferRing::updateDequeue(int current) {
	_dequeuePtr = current;
}

// ------------------------------------------------------------------------
// Controller::Device
// ------------------------------------------------------------------------

Controller::Device::Device(int portId, Controller *controller)
: _slotId{-1}, _portId{portId}, _controller{controller} {
}

arch::dma_pool *Controller::Device::setupPool() {
	return &_controller->_memoryPool;
}

arch::dma_pool *Controller::Device::bufferPool() {
	return &_controller->_memoryPool;
}

async::result<std::string> Controller::Device::configurationDescriptor() {
	arch::dma_object<ConfigDescriptor> header{&_controller->_memoryPool};
	co_await readDescriptor(header.view_buffer(), 0x0200);

	arch::dma_buffer descriptor{&_controller->_memoryPool, header->totalLength};
	co_await readDescriptor(descriptor, 0x0200);
	co_return std::string{(char *)descriptor.data(), descriptor.size()};
}

async::result<Configuration> Controller::Device::useConfiguration(int number) {
	auto descriptor = co_await configurationDescriptor();

	struct EndpointInfo {
		int pipe;
		PipeType dir;
		int packet_size;
		EndpointType type;
	};

	std::vector<EndpointInfo> _eps = {};

	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		(void)length;

		if(type != descriptor_type::endpoint)
			return;
		auto desc = (EndpointDescriptor *)p;

		// TODO: Pay attention to interface/alternative.
		auto packet_size = desc->maxPacketSize & 0x7FF;
		auto ep_type = info.endpointType.value();

		int pipe = info.endpointNumber.value();
		if (info.endpointIn.value()) {
			_eps.push_back({pipe, PipeType::in, packet_size, ep_type});
		} else {
			_eps.push_back({pipe, PipeType::out, packet_size, ep_type});
		}
	});

	for (auto &ep : _eps) {
		printf("xhci: setting up %s endpoint %d (max packet size: %d)\n", 
			ep.dir == PipeType::in ? "in" : "out", ep.pipe, ep.packet_size);
		co_await setupEndpoint(ep.pipe, ep.dir, ep.packet_size, ep.type);
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

	co_await ev.promise.async_get();

	if (ev.event.completionCode != 1)
		printf("xhci: failed to use configuration, completion code: '%s'\n",
			completionCodeNames[ev.event.completionCode]);

	printf("xhci: configuration set\n");

	co_return Configuration{std::make_shared<Controller::ConfigurationState>(_controller, shared_from_this(), number)};
}

async::result<void> Controller::Device::transfer(ControlTransfer info) {
	RawTrb setup_stage = {{
		0, static_cast<uint32_t>(info.buffer.size() << 16), 8,
		((info.flags == kXferToDevice ? 2 : 3) << 16) 
		| (1 << 6) | (static_cast<uint32_t>(TrbType::setupStage) << 10)}};

	memcpy(setup_stage.val, info.setup.data(), sizeof(SetupPacket));

	pushRawTransfer(0, setup_stage);

	size_t progress = 0;
	while(progress < info.buffer.size()) {
		uintptr_t pptr, ptr = (uintptr_t)info.buffer.data() + progress;
		HEL_CHECK(helPointerPhysical((void *)ptr, &pptr));

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

	co_await ev.promise.async_get();

	if (ev.event.completionCode != 1)
		printf("xhci: failed to perform a control transfer, completion code: '%s'\n",
			completionCodeNames[ev.event.completionCode]);
}

void Controller::Device::submit(int endpoint) {
	assert(_slotId != -1);
	_controller->ringDoorbell(_slotId, endpoint, /* stream */ 0);
}

async::result<void> Controller::Device::allocSlot(int slotType, int packetSize) {
	RawTrb enable_slot = {{0, 0, 0, 
		(slotType << 16)
			| (static_cast<uint32_t>(TrbType::enableSlotCommand) << 10)}};
	Controller::CommandRing::CommandEvent ev;
	_controller->_cmdRing.pushRawCommand(enable_slot, &ev);
	_controller->_cmdRing.submit();

	co_await ev.promise.async_get();

	assert(ev.event.completionCode != 9); // TODO: handle running out of device slots
	assert(ev.event.completionCode == 1); // success

	_slotId = ev.event.slotId;

	printf("xhci: slot enabled successfully!\n");
	printf("xhci: slot id for port %d is %d\n", _portId, _slotId);

	// initialize slot

	_devCtx = arch::dma_object<DeviceContext>{&_controller->_memoryPool};

	auto inputCtx = arch::dma_object<InputContext>{&_controller->_memoryPool};
	memset(inputCtx.data(), 0, sizeof(InputContext));
	inputCtx->icc.addContextFlags = (1 << 0) | (1 << 1); // slot and control endpoint
	// TODO: support hubs (generate route string)
	inputCtx->slotContext.val[0] = (1 << 27); // 1 context entry
	inputCtx->slotContext.val[1] = (_portId << 16); // root hub port

	_transferRings[0] = std::make_unique<TransferRing>(_controller);

	// type = control
	// max packet size = packetSize
	// max burst size = 0
	// tr dequeue = tr ring ptr
	// dcs = 1
	// interval = 0
	// max p streams = 0
	// mult = 0
	// error count = 3
	auto tr_ptr = _transferRings[0]->getPtr();
	printf("xhci: tr ptr = %016lx\n", tr_ptr);
	assert(!(tr_ptr & 0xF));
	inputCtx->endpointContext[0].val[1] = (3 << 1) | (4 << 3) | (packetSize << 16);
	inputCtx->endpointContext[0].val[2] = (1 << 0) | (tr_ptr & 0xFFFFFFF0);
	inputCtx->endpointContext[0].val[3] = (tr_ptr >> 32);

	uintptr_t dev_ctx_ptr;
	HEL_CHECK(helPointerPhysical(_devCtx.data(), &dev_ctx_ptr));
	_controller->_dcbaa[_slotId] = dev_ctx_ptr;

	uintptr_t in_ctx_ptr;
	HEL_CHECK(helPointerPhysical(inputCtx.data(), &in_ctx_ptr));

	RawTrb address_device = {{
		static_cast<uint32_t>(in_ctx_ptr & 0xFFFFFFFF),
		static_cast<uint32_t>(in_ctx_ptr >> 32), 0,
		(_slotId << 24) | 
			(static_cast<uint32_t>(TrbType::addressDeviceCommand) << 10)}};
	Controller::CommandRing::CommandEvent ev2;
	_controller->_cmdRing.pushRawCommand(address_device, &ev2);
	_controller->_cmdRing.submit();

	co_await ev2.promise.async_get();

	if (ev2.event.completionCode != 1)
		printf("xhci: failed to address device, completion code: '%s'\n",
			completionCodeNames[ev2.event.completionCode]);

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

	uintptr_t ptr;
	HEL_CHECK(helPointerPhysical(dest.data(), &ptr));

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

	co_await ev.promise.async_get();

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

async::result<void> Controller::Device::setupEndpoint(int endpoint, PipeType dir, size_t maxPacketSize, EndpointType type, bool drop) {
	printf("xhci: doing endpoint stuff to %d\n", endpoint);
	auto inputCtx = arch::dma_object<InputContext>{&_controller->_memoryPool};
	memset(inputCtx.data(), 0, sizeof(InputContext));

	int endpointId = endpoint * 2 + (dir == PipeType::in ? 1 : 0);

	printf("xhci: epId is %d\n", endpointId);

	inputCtx->icc.addContextFlags = (1 << 0) | (1 << (endpointId));
	inputCtx->slotContext = _devCtx->slotContext;

	inputCtx->slotContext.val[0] |= (31 << 27);

	_transferRings[endpointId - 1] = std::make_unique<TransferRing>(_controller);

	// max burst size = 0
	// tr dequeue = tr ring ptr
	// dcs = 1
	// interval = 0
	// max p streams = 0
	// mult = 0
	// error count = 3
	// average trb length = packet size * 2
	auto tr_ptr = _transferRings[endpointId - 1]->getPtr();
	printf("xhci: tr ptr = %016lx\n", tr_ptr);
	assert(!(tr_ptr & 0xF));
	inputCtx->endpointContext[endpointId - 1].val[1] = (3 << 1) | (getHcdEndpointType(dir, type) << 3) | (maxPacketSize << 16);
	inputCtx->endpointContext[endpointId - 1].val[2] = (1 << 0) | (tr_ptr & 0xFFFFFFF0);
	inputCtx->endpointContext[endpointId - 1].val[3] = (tr_ptr >> 32);
	inputCtx->endpointContext[endpointId - 1].val[4] = maxPacketSize * 2;

	uintptr_t in_ctx_ptr;
	HEL_CHECK(helPointerPhysical(inputCtx.data(), &in_ctx_ptr));

	RawTrb configure_endpoint = {{
		static_cast<uint32_t>(in_ctx_ptr & 0xFFFFFFFF),
		static_cast<uint32_t>(in_ctx_ptr >> 32), 0,
		(_slotId << 24) | 
			(static_cast<uint32_t>(TrbType::configureEndpointCommand) << 10)}};
	Controller::CommandRing::CommandEvent ev;
	_controller->_cmdRing.pushRawCommand(configure_endpoint, &ev);
	_controller->_cmdRing.submit();

	co_await ev.promise.async_get();

	if (ev.event.completionCode != 1)
		printf("xhci: failed to configure endpoint, completion code: '%s'\n",
			completionCodeNames[ev.event.completionCode]);

	assert(ev.event.completionCode == 1);

	printf("xhci: configure endpoint finished\n");
}

// ------------------------------------------------------------------------
// Controller::ConfigurationState
// ------------------------------------------------------------------------

Controller::ConfigurationState::ConfigurationState(Controller *controller, 
		std::shared_ptr<Device> device, int)
:_controller{controller}, _device{device} {
}

async::result<Interface> Controller::ConfigurationState::useInterface(int number, int alternative) {
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

async::result<Endpoint> Controller::InterfaceState::getEndpoint(PipeType type, int number) {
	co_return Endpoint{std::make_shared<Controller::EndpointState>(_controller, _device, number, type)};
}

// ------------------------------------------------------------------------
// Controller::EndpointState
// ------------------------------------------------------------------------

Controller::EndpointState::EndpointState(Controller *, 
		std::shared_ptr<Device> device, int endpoint, PipeType type)
: _device{device}, _endpoint{endpoint}, _type{type} {
}

async::result<void> Controller::EndpointState::transfer(ControlTransfer info) {
	assert(!"TODO: implement this");
	co_return;
}

async::result<size_t> Controller::EndpointState::transfer(InterruptTransfer info) {
	int endpointId = _endpoint * 2 + (_type == PipeType::in ? 1 : 0);

	Controller::TransferRing::TransferEvent ev;

	size_t progress = 0;
	while(progress < info.buffer.size()) {
		uintptr_t pptr, ptr = (uintptr_t)info.buffer.data() + progress;
		HEL_CHECK(helPointerPhysical((void *)ptr, &pptr));

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

	co_await ev.promise.async_get();

	assert(ev.event.completionCode == 1 || ev.event.completionCode == 13); // success

	co_return info.buffer.size() - ev.event.transferLen;
}

async::result<size_t> Controller::EndpointState::transfer(BulkTransfer info) {
	int endpointId = _endpoint * 2 + (_type == PipeType::in ? 1 : 0);

	Controller::TransferRing::TransferEvent ev;

	size_t progress = 0;
	while(progress < info.buffer.size()) {
		uintptr_t pptr, ptr = (uintptr_t)info.buffer.data() + progress;
		HEL_CHECK(helPointerPhysical((void *)ptr, &pptr));

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

	co_await ev.promise.async_get();

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
	auto irq = co_await device.accessIrq();
	co_await device.enableBusmaster();

	helix::Mapping mapping{bar, info.barInfo[0].offset, info.barInfo[0].length};

	auto controller = std::make_shared<Controller>(std::move(device), std::move(mapping),
			std::move(bar), std::move(irq));
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

	{
		async::queue_scope scope{helix::globalQueue()};
		observeControllers();
	}

	async::run_forever(helix::globalQueue()->run_token(), helix::currentDispatcher);

	return 0;
}
 
