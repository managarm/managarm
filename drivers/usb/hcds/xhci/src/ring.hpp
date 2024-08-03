#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include <protocols/usb/api.hpp>

#include <arch/dma_pool.hpp>

#include <async/result.hpp>
#include <async/sequenced-event.hpp>

#include <frg/expected.hpp>

#include "trb.hpp"

struct Event {
	TrbType type;
	int slotId;
	int completionCode;

	// Transfer and command completion events
	uintptr_t trbPointer;

	// Transfer event
	size_t transferLen;
	size_t endpointId;
	bool eventData;

	// Command completion event
	int commandCompletionParameter;

	// Port status change event
	size_t portId;

	// Device notification event
	uintptr_t notificationData;
	size_t notificationType;

	// Raw TRB
	RawTrb raw;

	static Event fromRawTrb(RawTrb trb);
	void printInfo();
};

inline frg::expected<protocols::usb::UsbError> completionToError(Event ev) {
	using protocols::usb::UsbError;

	switch (ev.completionCode) {
		case 1: return frg::success;
		case 13: return frg::success;
		case 3: return UsbError::babble;
		case 6: return UsbError::stall;
		case 22: return UsbError::unsupported;
		default: return UsbError::other;
	}
}

struct Controller;

struct EventRing {
	constexpr static size_t eventRingSize = 128;

	struct alignas(64) ErstEntry {
		uint32_t ringSegmentBaseLow;
		uint32_t ringSegmentBaseHi;
		uint32_t ringSegmentSize;
		uint32_t reserved;
	};

	struct alignas(64) EventRingEntries {
		RawTrb ent[eventRingSize];
	};

	static_assert(sizeof(ErstEntry) == 64, "invalid ErstEntry size");

	EventRing(Controller *controller);
	uintptr_t getErstPtr();
	uintptr_t getEventRingPtr();
	size_t getErstSize();

	void processRing();

private:
	arch::dma_object<EventRingEntries> _eventRing;
	arch::dma_array<ErstEntry> _erst;

	size_t _dequeuePtr;
	Controller *_controller;

	int _ccs;
};

struct ProducerRing {
	constexpr static size_t ringSize = 128;

	struct Transaction {
		async::result<frg::expected<protocols::usb::UsbError, size_t>>
		control(bool hasData);

		async::result<frg::expected<protocols::usb::UsbError, size_t>>
		normal();

		async::result<Event> command();

		void onEvent(Event event, RawTrb associatedTrb);
	private:
		std::vector<std::pair<RawTrb, Event>> events_;
		async::sequenced_event progressEvent_;
		uint64_t progressSeq_ = 0;

		async::result<frg::expected<protocols::usb::UsbError, std::pair<RawTrb, Event>>>
		nextEvent_() {
			co_await progressEvent_.async_wait(progressSeq_);
			auto ev = events_[progressSeq_++];
			FRG_CO_TRY(completionToError(ev.second));
			co_return ev;
		}
	};

	struct alignas(64) RingEntries {
		RawTrb ent[ringSize];
	};

	ProducerRing(Controller *controller);
	uintptr_t getPtr();
	size_t enqueuePtr() const { return _enqueuePtr; }
	bool producerCycle() const { return _pcs; }

	void pushRawTrb(RawTrb cmd, Transaction *tx);

	void processEvent(Event ev);

private:
	std::array<Transaction *, ringSize> _transactions;
	arch::dma_object<RingEntries> _ring;
	size_t _enqueuePtr;

	bool _pcs;

	void updateLink();
};
