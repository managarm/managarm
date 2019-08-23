
#include <queue>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>

#include "spec.hpp"

// ----------------------------------------------------------------
// controller.
// ----------------------------------------------------------------

struct Controller : std::enable_shared_from_this<Controller> {
	Controller(protocols::hw::Device hw_device,
			helix::Mapping mapping,
			helix::UniqueDescriptor mmio, helix::UniqueIrq irq);

	async::detached initialize();
	async::detached handleIrqs();

private:
	struct Event {
		// generic
		TrbType type;
		int slotId;
		int vfId;
		int completionCode;

		// transfer event specific
		uintptr_t trbPointer;
		size_t transferLen;
		size_t endpointId;
		bool eventData;

		// command completion event specific
		uintptr_t commandPointer;
		int commandCompletionParameter;

		// port status change event specific
		size_t portId;

		// doorbell event specific
		size_t doorbellReason;

		// device notification event specific
		uintptr_t notificationData;
		size_t notificationType;

		// raw trb
		RawTrb raw;

		static Event fromRawTrb(RawTrb trb);
		void printInfo();
	};

	struct CommandRing {
		constexpr static size_t commandRingSize = 128;

		struct CommandEvent {
			async::promise<void> promise;
			Event event;
		};

		struct alignas(64) CommandRingEntries {
			RawTrb ent[commandRingSize];
		};

		CommandRing(Controller *controller);
		uintptr_t getCrcr();

		void pushRawCommand(RawTrb cmd, CommandEvent *ev = nullptr);

		std::array<CommandEvent *, commandRingSize> _commandEvents;
		void submit();
	private:
		arch::dma_object<CommandRingEntries> _commandRing;
		size_t _dequeuePtr;
		size_t _enqueuePtr;

		Controller *_controller;

		bool _pcs; // producer cycle state
	};

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

		std::deque<Event> _dequeuedEvents;
		async::doorbell _doorbell;
	private:
		arch::dma_object<EventRingEntries> _eventRing;
		arch::dma_array<ErstEntry> _erst;

		void processEvent(Event ev);

		size_t _dequeuePtr;
		Controller *_controller;

		int _ccs;

	};

	struct Interrupter {
		Interrupter(int id, Controller *controller);
		void setEnable(bool enable);
		void setEventRing(EventRing *ring, bool clear_ehb = false);
		bool isPending();
		void clearPending();
	private:
		arch::mem_space _space;
	};

	protocols::hw::Device _hw_device;
	helix::Mapping _mapping;
	helix::UniqueDescriptor _mmio;
	helix::UniqueIrq _irq;
	arch::mem_space _space;
	arch::mem_space _operational;
	arch::mem_space _runtime;
	arch::mem_space _doorbells;

	void ringDoorbell(uint8_t doorbell, uint8_t target, uint16_t stream_id);

	uint16_t getExtendedCapabilityOffset(uint8_t id);

	arch::os::contiguous_pool _memoryPool;

	arch::dma_array<uint64_t> _dcbaa;
	arch::dma_array<uint64_t> _scratchpadBufArray;
	std::vector<arch::dma_buffer> _scratchpadBufs;

	std::vector<std::shared_ptr<Interrupter>> _interrupters;

	CommandRing _cmdRing;
	EventRing _eventRing;

	int _numPorts;
};

