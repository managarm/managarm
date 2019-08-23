
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
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

	_numPorts = _space.load(cap_regs::hcsparams1) & hcsparams1::max_ports;
	printf("xhci: %u ports\n", _numPorts);
}

uint16_t Controller::getExtendedCapabilityOffset(uint8_t id) {
	auto ptr = (_space.load(cap_regs::hccparams1) & hccparams1::ext_cap_ptr) * 4;
	if (!ptr)
		return 0;

	while(1) {
		auto val = arch::scalar_load<uint32_t>(_space, ptr);

		if (val == 0xFFFFFFFF)
			return 0;

		if (id == (val & 0xFF))
			return ptr;

		if (!((val >> 8) & 0xFF))
			return 0;

		ptr += ((val >> 8) & 0xFF) << 2;
	}

	return 0;
}

async::detached Controller::initialize() {
	auto usb_legacy_cap = getExtendedCapabilityOffset(0x1);

	if(usb_legacy_cap) {
		printf("xhci: usb legacy capability at %04x\n", usb_legacy_cap);

		if (arch::scalar_load<uint8_t>(_space, usb_legacy_cap + 2) & 1)
			printf("xhci: controller is currently owned by the BIOS\n");

		if(!(arch::scalar_load<uint8_t>(_space, usb_legacy_cap + 3) & 1)) {
			arch::scalar_store<uint8_t>(_space, usb_legacy_cap + 3, 
				arch::scalar_load<uint8_t>(_space, usb_legacy_cap + 3)
					| 1);
		} else {
			printf("xhci: we already own the controller\n");
		}

		while(arch::scalar_load<uint8_t>(_space, usb_legacy_cap + 2) & 1) {
			// Do nothing while we wait for the BIOS.
		}
		printf("xhci: took over controller from BIOS\n");
	} else {
		printf("xhci: no usb legacy support extended capability\n");
	}

	printf("xhci: initializing controller...\n");

	auto state = _operational.load(op_regs::usbcmd);
	state &= ~usbcmd::run;
	_operational.store(op_regs::usbcmd, state);

	while(!(_operational.load(op_regs::usbsts) & usbsts::hc_halted)); // wait for halt

	_operational.store(op_regs::usbcmd, usbcmd::hc_reset(1)); // reset hcd
	while(_operational.load(op_regs::usbsts) & usbsts::controller_not_ready); // poll for reset to complete
	printf("xhci: controller reset done...\n");

	_operational.store(op_regs::config, config::enabled_device_slots(1));

	auto hcsparams2 = _space.load(cap_regs::hcsparams2);
	auto max_scratchpad_bufs = 
		((hcsparams2 & hcsparams2::max_scratchpad_bufs_hi) << 4)
		| (hcsparams2 & hcsparams2::max_scratchpad_bufs_low);

	auto pagesize_reg = _operational.load(op_regs::pagesize);
	size_t page_size = 0;

	while(!(pagesize_reg & 1)) {
		pagesize_reg >>= 1;
		page_size++;
	}

	page_size = 1 << (page_size + 12); // 2^(n + 12)

	printf("xhci: max scratchpad buffers: %u\n", max_scratchpad_bufs);
	printf("xhci: page size: %lu\n", page_size);

	auto max_erst = _space.load(cap_regs::hcsparams2) & hcsparams2::erst_max;
	max_erst = 1 << (max_erst);// | 2^max_erst
	max_erst = max_erst ? : 1; // /
	printf("xhci: max_erst: %u\n", max_erst);

	_scratchpadBufArray = arch::dma_array<uint64_t>{
		&_memoryPool, static_cast<size_t>(max_scratchpad_bufs)};
	for (size_t i = 0; i < max_scratchpad_bufs; i++) {
		auto buf = std::make_shared<arch::dma_buffer>(&_memoryPool,
								page_size);
		_scratchpadBufs.push_back(buf);

		uintptr_t phys;
		HEL_CHECK(helPointerPhysical(buf->data(), &phys));
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
	auto max_intrs = _space.load(cap_regs::hcsparams1) & hcsparams1::max_intrs;
	printf("xhci: max interrupters: %u\n", max_intrs);

	_interrupters.push_back(std::make_shared<Interrupter>(0, this));

	for (int i = 1; i < max_intrs; i++)
		_interrupters.push_back(std::make_shared<Interrupter>(i, this));

	_interrupters[0]->setEventRing(&_eventRing);
	_interrupters[0]->setEnable(true);

	handleIrqs();

	_operational.store(op_regs::usbcmd, usbcmd::run(1) | usbcmd::intr_enable(1)); // enable interrupts and start hcd

	_operational.store(op_regs::usbsts, usbsts::event_intr(1));

	while(_operational.load(op_regs::usbsts) & usbsts::hc_halted); // wait for start

	printf("xhci: init done...\n");

	printf("xhci: command ring test:\n");

	RawTrb disable_slot_1_cmd = {{0, 0, 0, (1 << 24) | (10 << 10)}};
	Controller::CommandRing::CommandEvent ev;
	_cmdRing.pushRawCommand({{0, 0, 0, (23 << 10)}});
	_cmdRing.pushRawCommand(disable_slot_1_cmd, &ev);
	printf("xhci: submitting a disable slot 1 command\n");
	_cmdRing.submit();

	co_await ev.promise.async_get();

	auto compCode = ev.event.completionCode;
	printf("xhci: received response to command:\n");
	printf("xhci: response completion code: (%s) %u\n", 
			completionCodeNames[compCode], compCode);

	if (compCode != 1 && compCode != 11) {
		printf("xhci: invalid response to command (hardware/emulator quirk)\n");
		printf("xhci: was expecting either: %s (1) or %s (11)\n",
					completionCodeNames[1],
					completionCodeNames[11]);
		printf("xhci: command ring test not successful!\n");
	} else {
		printf("xhci: command ring test successful!\n");
	}

	co_return;
}

async::detached Controller::handleIrqs() {
	co_await _hw_device.enableBusIrq();

	uint64_t sequence = 0;

	while(1) {
		helix::AwaitEvent await;
		auto &&submit = helix::submitAwaitEvent(_irq, &await, sequence,
				helix::Dispatcher::global());
		co_await submit.async_wait();
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
:_commandRing{&controller->_memoryPool}, _dequeuePtr{0}, _enqueuePtr{0},
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
	return ptr + _dequeuePtr * sizeof(RawTrb);
}

uintptr_t Controller::EventRing::getEventRingPtr() {
	uintptr_t ptr;
	HEL_CHECK(helPointerPhysical(_eventRing.data(), &ptr));
	return ptr;
}

size_t Controller::EventRing::getErstSize() {
	return _erst.size();
}

void Controller::EventRing::processRing() {
	while(1) {
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
	_doorbell.ring();
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
	_space.store(interrupter::erstsz, ring->getErstSize());
	_space.store(interrupter::erstba_low,ring->getErstPtr() & 0xFFFFFFFF);
	_space.store(interrupter::erstba_hi, ring->getErstPtr() >> 32);

	_space.store(interrupter::erdp_low,
		(ring->getEventRingPtr() & 0xFFFFFFF0) | (clearEhb << 3));
	_space.store(interrupter::erdp_hi, ring->getEventRingPtr() >> 32);
}

bool Controller::Interrupter::isPending() {
	return _space.load(interrupter::iman) & iman::pending;
}

void Controller::Interrupter::clearPending() {
	_space.store(interrupter::iman, iman::pending(1));
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
	printf("xhci: completion code: %d\n", completionCode);

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
// Freestanding PCI discovery functions.
// ------------------------------------------------------------------------

async::detached bindController(mbus::Entity entity) {
	protocols::hw::Device device(co_await entity.bind());
	auto info = co_await device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = co_await device.accessBar(0);
	auto irq = co_await device.accessIrq();

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

	helix::globalQueue()->run();

	return 0;
}
 
