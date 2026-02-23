#include "ring.hpp"
#include "xhci.hpp"

// ------------------------------------------------------------------------
// Event
// ------------------------------------------------------------------------

constexpr const char *trbTypeNames[] = {
	"Reserved",

	"Normal",
	"Setup stage",
	"Data stage",
	"Status stage",
	"Isochronous",
	"Link",
	"Event data",
	"No Op (transfer)",

	"Enable slot",
	"Disable slot",
	"Address device",
	"Configure endpoint",
	"Evaluate context",
	"Reset endpoint",
	"Stop endpoint",
	"Set TR dequeue pointer",
	"Reset device",
	"Force event",
	"Negotiate bandwidth",
	"Set latency tolerance value",
	"Get port bandwidth",
	"Force header",
	"No Op (command)",
	"Get extended property",
	"Set extended property",

	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",
	"Reserved",

	"Transfer event",
	"Command completion event",
	"Port status change event",
	"Bandwidth request event",
	"Doorbell event",
	"Host controller event",
	"Device notification event",
	"MFINDEX wrap event"
};

Event Event::fromRawTrb(RawTrb trb) {
	Event ev;

	ev.type = static_cast<TrbType>((trb.val[3] >> 10) & 63);
	ev.completionCode = static_cast<CompletionCode>((trb.val[2] >> 24) & 0xFF);
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
	printf("xhci: Type: %s (%u)\n",
			trbTypeNames[static_cast<unsigned int>(type)],
			static_cast<unsigned int>(type));
	printf("xhci: Slot ID: %d\n", slotId);
	printf("xhci: Completion code: %s (%d)\n",
			completionCodeName(),
			static_cast<int>(completionCode));

	switch(type) {
		case TrbType::transferEvent:
			printf("xhci: TRB pointer: %016lx, transfer length %lu\n", trbPointer,
					transferLen);
			printf("xhci: Endpoint ID: %lu, has event data? %s\n",
					endpointId, eventData ? "yes" : "no");
			break;
		case TrbType::commandCompletionEvent:
			printf("xhci: TRB pointer: %016lx\n", trbPointer);
			printf("xhci: Command completion parameter: %d\n",
					commandCompletionParameter);
			break;
		case TrbType::portStatusChangeEvent:
			printf("xhci: Port ID: %lu\n", portId);
			break;
		case TrbType::bandwidthRequestEvent:
		case TrbType::doorbellEvent:
		case TrbType::hostControllerEvent:
		case TrbType::mfindexWrapEvent:
			break;
		case TrbType::deviceNotificationEvent:
			printf("xhci: Notification data: %lx\n",
					notificationData);
			printf("xhci: Notification type: %lu\n",
					notificationType);
			break;
		default:
			printf("xhci: Invalid event\n");
	}

	printf("xhci: --- End of event dump ---\n");
}

const char *Event::completionCodeName() const {
	return completionCodeNames[static_cast<int>(completionCode)];
}

// ------------------------------------------------------------------------
// EventRing
// ------------------------------------------------------------------------

EventRing::EventRing(Controller *controller)
: _eventRing{controller->memoryPool()}, _erst{controller->memoryPool(), 1}
, _controller{controller}, _dequeue{0, true} {
	for (size_t i = 0; i < eventRingSize; i++) {
		_eventRing->ent[i] = {{0, 0, 0, 0}};
	}

	_erst[0].ringSegmentBaseLow = getEventRingPtr() & 0xFFFFFFFF;
	_erst[0].ringSegmentBaseHi = getEventRingPtr() >> 32;
	_erst[0].ringSegmentSize = eventRingSize;
	_erst[0].reserved = 0;

	_controller->barrier.writeback(_eventRing.view_buffer());
	_controller->barrier.writeback(_erst.view_buffer());
}

uintptr_t EventRing::getErstPtr() {
	return helix::ptrToPhysical(_erst.data());
}

uintptr_t EventRing::getEventRingPtr() {
	return helix::ptrToPhysical(_eventRing.data()) + _dequeue.index * sizeof(RawTrb);
}

size_t EventRing::getErstSize() {
	return _erst.size();
}

void EventRing::processRing() {
	_controller->barrier.invalidate(_eventRing.view_buffer());
	while((_eventRing->ent[_dequeue.index].val[3] & 1) == _dequeue.cycle) {
		RawTrb rawEv = _eventRing->ent[_dequeue.index];

		_dequeue.advance(1, eventRingSize);

		Event ev = Event::fromRawTrb(rawEv);
		_controller->processEvent(ev);
	}
}

// ------------------------------------------------------------------------
// ProducerRing
// ------------------------------------------------------------------------

ProducerRing::ProducerRing(Controller *controller)
: _transactions{}, _ring{controller->memoryPool()}, _controller{controller}
, _enqueue{0, true}, _dequeue{0, true} {
	for (size_t i = 0; i < ringSize; i++) {
		_ring->ent[i] = {{0, 0, 0, 0}};
	}

	_controller->barrier.writeback(_ring.view_buffer());
}

uintptr_t ProducerRing::getPtr() {
	auto ptr = helix::ptrToPhysical(_ring.data());
	assert(!(ptr & 63));
	return ptr;
}

void ProducerRing::processEvent(Event ev) {
	assert(ev.type == TrbType::commandCompletionEvent
			|| ev.type == TrbType::transferEvent);

	size_t idx = (ev.trbPointer - getPtr()) / sizeof(RawTrb);
	assert(idx < ringSize);

	auto trb = _ring->ent[idx];
	auto tx = std::exchange(_transactions[idx], nullptr);

	retire({idx, bool(trb.val[3] & 1)});

	if (tx) {
		tx->onEvent(_controller, ev, trb);
	}
}

async::result<RingPointer> ProducerRing::pushTrbs(const std::vector<RawTrb> &trbs, Transaction *tx) {
	assert(trbs.size() <= usableRingSize);

	while (true) {
		co_await _progressEvent.async_wait_if([&] {
			std::unique_lock lock{_mutex};
			return usableRingSize - inFlight() < trbs.size();
		});

		std::unique_lock lock{_mutex};

		if (usableRingSize - inFlight() < trbs.size())
			continue;

		lock.release();
		break;
	}

	std::unique_lock lock{_mutex, std::adopt_lock};

	auto initialPtr = _enqueue;
	auto finalPtr = _enqueue;
	for (size_t i = 0; i < trbs.size(); i++) {
		auto trb = trbs[i];
		// Post all TRBs, except use the incorrect cycle bit for the first.
		// This is to prevent the controller from potentially starting a TD we
		// are still writing TRBs for (and failing because it finishes early).
		trb.val[3] |= i > 0 ? uint32_t{_enqueue.cycle} : uint32_t{!_enqueue.cycle};

		_transactions[_enqueue.index] = tx;
		_ring->ent[_enqueue.index] = trb;

		finalPtr = _enqueue;
		_enqueue.advance(1, usableRingSize);
	}

	_updateLink(initialPtr.cycle);

	// Make sure this is all visible to the controller.
	_controller->barrier.writeback(_ring.view_buffer());

	// TODO(qookie): Write barrier here.

	// Now, update the first TRB to the correct cycle state.
	auto v = _ring->ent[initialPtr.index].val[3];
	v &= ~uint32_t{1};
	v |= uint32_t{initialPtr.cycle};
	_ring->ent[initialPtr.index].val[3] = v;

	_controller->barrier.writeback(_ring.view_buffer());

	co_return finalPtr;
}

void ProducerRing::retire(RingPointer newDequeue) {
	std::unique_lock lock{_mutex};

	// Make sure we don't accidentally rewind the dequeue.
	assert(newDequeue.cycle != _dequeue.cycle || newDequeue.index >= _dequeue.index);
	// Make sure the dequeue is at or before enqueue.
	assert(newDequeue.cycle != _enqueue.cycle || newDequeue.index <= _enqueue.index);

	_dequeue = newDequeue;
	_progressEvent.raise();
}

void ProducerRing::_updateLink(bool initialCycle) {
	if (_enqueue.cycle == initialCycle) return;

	_ring->ent[ringSize - 1] = {{
		static_cast<uint32_t>(getPtr() & 0xFFFFFFFF),
		static_cast<uint32_t>(getPtr() >> 32),
		0,
		static_cast<uint32_t>(initialCycle | (1 << 1) | (1 << 5) | (6 << 10))
	}};
}

// NOTE(qookie): The logic as far as I understand is as follows.
// There are 3 cases to consider that cause events to be generated:
// 1. Successful completion of the whole chain or short packet at the
//    end. Only one event is produced for the final TRB.
// 2. Short packet in the middle of the chain. Two events are
//    produced: one for the TRB that got the short packet, and one
//    for the final TRB that has IOC, so we also need to wait for the
//    latter one in that case.
//    TODO: The second packet not be generated by controllers
//          implementing xHCI 0.96 or older.
// 3. Other error completion. This causes the endpoint to go into the
//    halted state, and only one event is produced for the failing
//    TRB, hence we do not need to wait for any other TRB and can
//    bail out early via FRG_CO_TRY.

// TODO(qookie): The logic in transfer() might not work for isochronous
// endpoints (which we don't support yet) on some controllers (e.g.
// NEC ones). According to the Linux driver, if a TRB in the middle of
// an isoch TD fails, the controller carries on (as it should), but no
// event is generated for the final TRB in the chain (the one with IOC
// set). Other controllers do generate two events though.
async::result<frg::expected<protocols::usb::UsbError, size_t>>
ProducerRing::Transaction::transfer(bool allowShortCompletion) {
	// This will either be a short packet completion for a data stage/normal TRB,
	// the success completion for the final TRB in the TD, or an error completion.
	auto [trb, ev] = FRG_CO_TRY(co_await nextEvent_());

	if (ev.completionCode == CompletionCode::shortPacket) {
		if (allowShortCompletion) {
			assert(trb.val[3] & (1 << 4));
		}

		std::tie(trb, ev) = FRG_CO_TRY(co_await nextEvent_());

		if (!allowShortCompletion) {
			co_return proto::UsbError::shortPacket;
		}
	}

	co_return ev.transferLen;
}

async::result<Event> ProducerRing::Transaction::command() {
	co_await progressEvent_.async_wait(progressSeq_);
	co_return events_[progressSeq_++].second;
}

void ProducerRing::Transaction::onEvent(Controller *controller, Event event, RawTrb associatedTrb) {
	if (event.completionCode != CompletionCode::success) {
		auto associatedTrbType = static_cast<TrbType>((associatedTrb.val[3] >> 10) & 63);

		// Ignore short packet completions for transfers
		if (event.type == TrbType::transferEvent && event.completionCode != CompletionCode::shortPacket) {
			std::cout << controller << "Transfer TRB '" << trbTypeNames[static_cast<int>(associatedTrbType)] << "'"
				<< " completed with '" << event.completionCodeName() << "'"
				<< " (Slot " << event.slotId << ", EP " << event.endpointId << ")" << std::endl;
		} else if (event.type == TrbType::commandCompletionEvent) {
			std::cout << controller << "Command TRB '" << trbTypeNames[static_cast<int>(associatedTrbType)] << "'"
				<< " completed with '" << event.completionCodeName() << "'" << std::endl;
		}
	}

	events_.push_back({associatedTrb, event});
	progressEvent_.raise();
}
