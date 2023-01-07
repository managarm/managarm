#pragma once

#include <cstdint>
#include <cstddef>

#include <arch/dma_pool.hpp>
#include <async/oneshot-event.hpp>

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

	struct Completion {
		async::oneshot_event completion;
		Event event;
	};

	struct alignas(64) RingEntries {
		RawTrb ent[ringSize];
	};

	ProducerRing(Controller *controller);
	uintptr_t getPtr();

	void pushRawTrb(RawTrb cmd, Completion *comp = nullptr);

	void processEvent(Event ev);

private:
	std::array<Completion *, ringSize> _completions;
	arch::dma_object<RingEntries> _ring;
	size_t _enqueuePtr;

	bool _pcs;

	void updateLink();
};
