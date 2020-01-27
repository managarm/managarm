
#include <queue>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>
#include <helix/timer.hpp>
#include <protocols/usb/api.hpp>

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

	struct TransferRing {
		constexpr static size_t transferRingSize = 128;

		struct TransferEvent {
			async::promise<void> promise;
			Event event;
		};

		struct alignas(64) TransferRingEntries {
			RawTrb ent[transferRingSize];
		};

		TransferRing(Controller *controller);
		uintptr_t getPtr();

		void pushRawTransfer(RawTrb cmd, TransferEvent *ev = nullptr);

		void updateDequeue(int current);
		void updateLink();

		std::array<TransferEvent *, transferRingSize> _transferEvents;
	private:
		arch::dma_object<TransferRingEntries> _transferRing;
		size_t _dequeuePtr;
		size_t _enqueuePtr;

		Controller *_controller;

		bool _pcs; // producer cycle state
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

	struct Device;
	struct SupportedProtocol;

	struct Port {
		Port(int id, Controller *controller, SupportedProtocol *port);
		void reset();
		void disable();
		void resetChangeBits();
		bool isConnected();
		bool isEnabled();
		bool isPowered();
		void transitionToLinkStatus(uint8_t status);
		async::detached initPort();

		template <typename T>
		async::result<void> awaitFlag(arch::field<uint32_t, T> field, T value) {
			while (true) {
				resetChangeBits();
				if ((_space.load(port::portsc) & field) == value)
					co_return;

				async::cancellation_event ev;
				helix::TimeoutCancellation tc{1'000'000'000, ev};

				co_await _doorbell.async_wait(ev);
				co_await tc.retire();
			}
		}

		async::doorbell _doorbell;
	private:
		uint8_t getLinkStatus();
		uint8_t getSpeed();
		int _id;
		std::shared_ptr<Device> _device;
		Controller *_controller;
		SupportedProtocol *_proto;
		arch::mem_space _space;
	};

	struct Device : DeviceData, std::enable_shared_from_this<Device> {
		Device(int portId, Controller *controller);

		// Public API inherited from DeviceData.
		arch::dma_pool *setupPool() override;
		arch::dma_pool *bufferPool() override;
		async::result<std::string> configurationDescriptor() override;
		async::result<Configuration> useConfiguration(int number) override;
		async::result<void> transfer(ControlTransfer info) override;

		void submit(int endpoint);
		void pushRawTransfer(int endpoint, RawTrb cmd, TransferRing::TransferEvent *ev = nullptr);
		async::result<void> allocSlot(int slotType, int packetSize);

		async::result<void> readDescriptor(arch::dma_buffer_view dest, uint16_t desc);

		std::array<std::unique_ptr<TransferRing>, 31> _transferRings;

		int _slotId;

		async::result<void> setupEndpoint(int endpoint, PipeType dir, size_t maxPacketSize, EndpointType type, bool drop = false);

	private:
		int _portId;
		Controller *_controller;

		arch::dma_object<DeviceContext> _devCtx;
	};

	struct SupportedProtocol {
		int minor;
		int major;

		std::string name;

		size_t compatiblePortStart;
		size_t compatiblePortCount;

		uint16_t protocolDefined;

		size_t protocolSlotType;

		struct PortSpeed {
			uint8_t value;
			uint8_t exponent;
			uint8_t type;

			bool fullDuplex;

			uint8_t linkProtocol;
			uint16_t mantissa;
		};

		std::vector<PortSpeed> speeds;
	};

	struct ConfigurationState final : ConfigurationData {
		explicit ConfigurationState(Controller *controller, std::shared_ptr<Device> device, int number);

		async::result<Interface> useInterface(int number, int alternative) override;

	private:
		Controller *_controller;
		std::shared_ptr<Device> _device;
		int _number;
	};

	struct InterfaceState final : InterfaceData {
		explicit InterfaceState(Controller *controller, std::shared_ptr<Device> device, int interface);

		async::result<Endpoint> getEndpoint(PipeType type, int number) override;

	private:
		Controller *_controller;
		std::shared_ptr<Device> _device;
		int _interface;
	};

	struct EndpointState final : EndpointData {
		explicit EndpointState(Controller *controller, std::shared_ptr<Device> device, int endpoint, PipeType type);

		async::result<void> transfer(ControlTransfer info) override;
		async::result<size_t> transfer(InterruptTransfer info) override;
		async::result<size_t> transfer(BulkTransfer info) override;

	private:
		Controller *_controller;
		std::shared_ptr<Device> _device;
		int _endpoint;
		PipeType _type;
	};

	std::vector<SupportedProtocol> _supportedProtocols;

	protocols::hw::Device _hw_device;
	helix::Mapping _mapping;
	helix::UniqueDescriptor _mmio;
	helix::UniqueIrq _irq;
	arch::mem_space _space;
	arch::mem_space _operational;
	arch::mem_space _runtime;
	arch::mem_space _doorbells;

	void ringDoorbell(uint8_t doorbell, uint8_t target, uint16_t stream_id);

	std::vector<std::pair<uint8_t, uint16_t>> getExtendedCapabilityOffsets();

	arch::os::contiguous_pool _memoryPool;

	arch::dma_array<uint64_t> _dcbaa;
	arch::dma_array<uint64_t> _scratchpadBufArray;
	std::vector<arch::dma_buffer> _scratchpadBufs;

	std::vector<std::unique_ptr<Interrupter>> _interrupters;
	std::vector<std::unique_ptr<Port>> _ports;
	std::array<std::shared_ptr<Device>, 256> _devices;

	CommandRing _cmdRing;
	EventRing _eventRing;

	int _numPorts;
	int _maxDeviceSlots;
};


