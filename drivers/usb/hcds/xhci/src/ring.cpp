#include "ring.hpp"
#include "xhci.hpp"

// ------------------------------------------------------------------------
// Event
// ------------------------------------------------------------------------

Event Event::fromRawTrb(RawTrb trb) {
	Event ev;

	ev.type = static_cast<TrbType>((trb.val[3] >> 10) & 63);
	ev.completionCode = (trb.val[2] >> 24) & 0xFF;
	ev.slotId = (trb.val[3] >> 24) & 0xFF;
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
			ev.trbPointer = trb.val[0] |
				(static_cast<uintptr_t>(trb.val[1]) << 32);

			ev.commandCompletionParameter = trb.val[2] & 0xFFFFFF;
			break;

		case TrbType::portStatusChangeEvent:
			ev.portId = (trb.val[0] >> 24) & 0xFF;
			break;

		case TrbType::deviceNotificationEvent:
			ev.notificationData = (trb.val[0] |
				(static_cast<uintptr_t>(trb.val[1]) << 32))
				>> 8;
			ev.notificationType = (trb.val[0] >> 4) & 0xF;
			break;

		default:
			printf("xhci: Unexpected event 0x%02x in Event::fromRawTrb, ignoring...\n",
					static_cast<uint32_t>(ev.type));
	}

	return ev;
}

void Event::printInfo() {
	printf("xhci: --- Event dump ---\n");
	printf("xhci: Raw: %08x %08x %08x %08x\n",
			raw.val[0], raw.val[1], raw.val[2], raw.val[3]);
	printf("xhci: Type: %u\n", static_cast<unsigned int>(type));
	printf("xhci: Slot ID: %d\n", slotId);
	printf("xhci: Completion code: %s (%d)\n",
			completionCodeNames[completionCode],
			completionCode);

	switch(type) {
		case TrbType::transferEvent:
			printf("xhci: Type name: Transfer Event\n");
			printf("xhci: TRB pointer: %016lx, transfer length %lu\n", trbPointer,
					transferLen);
			printf("xhci: Endpoint ID: %lu, has event data? %s\n",
					endpointId, eventData ? "yes" : "no");
			break;
		case TrbType::commandCompletionEvent:
			printf("xhci: Type name: Command Completion Event\n");
			printf("xhci: TRB pointer: %016lxn", trbPointer);
			printf("xhci: Command completion parameter: %d\n",
					commandCompletionParameter);
			break;
		case TrbType::portStatusChangeEvent:
			printf("xhci: Type name: Port Status Change Event\n");
			printf("xhci: Port ID: %lu\n", portId);
			break;
		case TrbType::bandwidthRequestEvent:
			printf("xhci: Type name: Bandwidth Request Event\n");
			break;
		case TrbType::doorbellEvent:
			printf("xhci: Type name: Doorbell Event\n");
			break;
		case TrbType::hostControllerEvent:
			printf("xhci: Type name: Host Controller Event\n");
			break;
		case TrbType::deviceNotificationEvent:
			printf("xhci: Type name: Device Notification Event\n");
			printf("xhci: Notification data: %lx\n",
					notificationData);
			printf("xhci: Notification type: %lu\n",
					notificationType);
			break;
		case TrbType::mfindexWrapEvent:
			printf("xhci: Type name: MFINDEX Wrap Event\n");
			break;
		default:
			printf("xhci: Invalid event\n");
	}

	printf("xhci: --- End of event dump ---\n");
}

// ------------------------------------------------------------------------
// EventRing
// ------------------------------------------------------------------------

EventRing::EventRing(Controller *controller)
: _eventRing{controller->memoryPool()}, _erst{controller->memoryPool(), 1},
	_dequeuePtr{0}, _controller{controller}, _ccs{1} {

	for (size_t i = 0; i < eventRingSize; i++) {
		_eventRing->ent[i] = {{0, 0, 0, 0}};
	}

	_erst[0].ringSegmentBaseLow = getEventRingPtr() & 0xFFFFFFFF;
	_erst[0].ringSegmentBaseHi = getEventRingPtr() >> 32;
	_erst[0].ringSegmentSize = eventRingSize;
	_erst[0].reserved = 0;
}

uintptr_t EventRing::getErstPtr() {
	return helix::ptrToPhysical(_erst.data());
}

uintptr_t EventRing::getEventRingPtr() {
	return helix::ptrToPhysical(_eventRing.data()) + _dequeuePtr * sizeof(RawTrb);
}

size_t EventRing::getErstSize() {
	return _erst.size();
}

void EventRing::processRing() {
	while((_eventRing->ent[_dequeuePtr].val[3] & 1) == _ccs) {
		RawTrb rawEv = _eventRing->ent[_dequeuePtr];

		int oldCcs = _ccs;

		_dequeuePtr++;
		if (_dequeuePtr >= eventRingSize) {
			_dequeuePtr = 0; // Wrap around
			_ccs = !_ccs; // Invert cycle state
		}

		if ((rawEv.val[3] & 1) != oldCcs)
			break; // Not the proper cycle state

		Event ev = Event::fromRawTrb(rawEv);
		_controller->processEvent(ev);
	}
}

// ------------------------------------------------------------------------
// ProducerRing
// ------------------------------------------------------------------------

ProducerRing::ProducerRing(Controller *controller)
: _completions{}, _ring{controller->memoryPool()}, _enqueuePtr{0}, _pcs{true} {
	for (uint32_t i = 0; i < ringSize; i++) {
		_ring->ent[i] = {{0, 0, 0, 0}};
	}

	updateLink();
}

uintptr_t ProducerRing::getPtr() {
	return helix::ptrToPhysical(_ring.data());
}

void ProducerRing::pushRawTrb(RawTrb cmd, Completion *comp) {
	_ring->ent[_enqueuePtr] = cmd;
	_completions[_enqueuePtr] = comp;

	if (_pcs) {
		_ring->ent[_enqueuePtr].val[3] |= 1;
	} else {
		_ring->ent[_enqueuePtr].val[3] &= ~1;
	}

	_enqueuePtr++;

	if (_enqueuePtr >= ringSize - 1) {
		updateLink();
		_pcs = !_pcs;
		_enqueuePtr = 0;
	}
}

void ProducerRing::processEvent(Event ev) {
	assert(ev.type == TrbType::commandCompletionEvent
			|| ev.type == TrbType::transferEvent);

	size_t idx = (ev.trbPointer - getPtr()) / sizeof(RawTrb);
	assert(idx < ringSize);

	auto comp = std::exchange(_completions[idx], nullptr);

	if (comp) {
		comp->event = ev;
		comp->completion.raise();
	}
}

void ProducerRing::updateLink() {
	_ring->ent[ringSize - 1] = {{
		static_cast<uint32_t>(getPtr() & 0xFFFFFFFF),
		static_cast<uint32_t>(getPtr() >> 32),
		0,
		static_cast<uint32_t>(_pcs | (1 << 1) | (1 << 5) | (6 << 10))
	}};
}
