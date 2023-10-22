
#include <queue>

#include <arch/mem_space.hpp>
#include <arch/dma_pool.hpp>
#include <async/recurring-event.hpp>
#include <async/sequenced-event.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>
#include <helix/timer.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/hub.hpp>
#include <protocols/hw/client.hpp>

#include "spec.hpp"
#include "context.hpp"
#include "trb.hpp"
#include "ring.hpp"

namespace proto = protocols::usb;

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

struct Controller final : proto::BaseController {
	Controller(protocols::hw::Device hw_device,
			mbus::Entity entity,
			helix::Mapping mapping,
			helix::UniqueDescriptor mmio,
			helix::UniqueIrq irq, bool useMsis);

	virtual ~Controller() = default;

	async::detached initialize();
	async::detached handleIrqs();
	async::detached handleMsis();

	async::result<void> enumerateDevice(std::shared_ptr<proto::Hub> hub, int port, proto::DeviceSpeed speed) override;

	arch::os::contiguous_pool *memoryPool() {
		return &_memoryPool;
	}

	void processEvent(Event ev);

private:
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

		async::recurring_event _doorbell;

		async::result<proto::PortState> pollState();
		async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> getGenericSpeed();

	private:
		uint8_t getLinkStatus();
		uint8_t getSpeed();
		int _id;
		std::shared_ptr<Device> _device;
		Controller *_controller;
		SupportedProtocol *_proto;
		arch::mem_space _space;

		async::sequenced_event _pollEv;
		uint64_t _pollSeq = 0;
		proto::PortState _state{};
	};

	struct RootHub final : proto::Hub {
		RootHub(Controller *controller, SupportedProtocol &proto, mbus::Entity entity);

		size_t numPorts() override;
		async::result<proto::PortState> pollState(int port) override;
		async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> issueReset(int port) override;

		SupportedProtocol *protocol() {
			return _proto;
		}

		auto entityId() {
			return _entity.getId();
		}

	private:
		Controller *_controller;
		SupportedProtocol *_proto;
		std::vector<std::unique_ptr<Port>> _ports;
		mbus::Entity _entity;
	};

	struct Device final : proto::DeviceData, std::enable_shared_from_this<Device> {
		Device(Controller *controller);

		// Public API inherited from DeviceData.
		arch::dma_pool *setupPool() override;
		arch::dma_pool *bufferPool() override;
		async::result<frg::expected<proto::UsbError, std::string>> deviceDescriptor() override;
		async::result<frg::expected<proto::UsbError, std::string>> configurationDescriptor() override;
		async::result<frg::expected<proto::UsbError, proto::Configuration>> useConfiguration(int number) override;
		async::result<frg::expected<proto::UsbError>> transfer(proto::ControlTransfer info) override;

		void submit(int endpoint);
		void pushRawTransfer(int endpoint, RawTrb cmd, ProducerRing::Completion *ev = nullptr);

		async::result<frg::expected<proto::UsbError>> enumerate(size_t rootPort, size_t port, uint32_t route, std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed, int slotType);

		async::result<frg::expected<proto::UsbError>> readDescriptor(arch::dma_buffer_view dest, uint16_t desc);

		std::array<std::unique_ptr<ProducerRing>, 31> _transferRings;

		async::result<frg::expected<proto::UsbError>> setupEndpoint(int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type);

		async::result<frg::expected<proto::UsbError>> configureHub(std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed);

		size_t slot() const {
			return _slotId;
		}

	private:
		int _slotId;

		Controller *_controller;

		DeviceContext _devCtx;

		void _initEpCtx(InputContext &ctx, int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type);
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

	struct ConfigurationState final : proto::ConfigurationData {
		explicit ConfigurationState(Controller *controller, std::shared_ptr<Device> device, int number);

		async::result<frg::expected<proto::UsbError, proto::Interface>>
		useInterface(int number, int alternative) override;

	private:
		Controller *_controller;
		std::shared_ptr<Device> _device;
	};

	struct InterfaceState final : proto::InterfaceData {
		explicit InterfaceState(Controller *controller, std::shared_ptr<Device> device, int interface);

		async::result<frg::expected<proto::UsbError, proto::Endpoint>>
		getEndpoint(proto::PipeType type, int number) override;

	private:
		Controller *_controller;
		std::shared_ptr<Device> _device;
	};

	struct EndpointState final : proto::EndpointData {
		explicit EndpointState(Controller *controller, std::shared_ptr<Device> device, int endpoint, proto::PipeType type);

		async::result<frg::expected<proto::UsbError>> transfer(proto::ControlTransfer info) override;
		async::result<frg::expected<proto::UsbError, size_t>> transfer(proto::InterruptTransfer info) override;
		async::result<frg::expected<proto::UsbError, size_t>> transfer(proto::BulkTransfer info) override;

	private:
		std::shared_ptr<Device> _device;
		int _endpoint;
		proto::PipeType _type;
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

	async::result<Event> submitCommand(RawTrb trb) {
		ProducerRing::Completion comp;
		_cmdRing.pushRawTrb(trb, &comp);

		ringDoorbell(0, 0, 0);

		co_await comp.completion.wait();

		co_return comp.event;
	}

	arch::os::contiguous_pool _memoryPool;

	arch::dma_array<uint64_t> _dcbaa;
	arch::dma_array<uint64_t> _scratchpadBufArray;
	std::vector<arch::dma_buffer> _scratchpadBufs;

	std::vector<std::unique_ptr<Interrupter>> _interrupters;
	std::vector<Port *> _ports;
	std::array<std::shared_ptr<Device>, 256> _devices;

	std::vector<std::shared_ptr<RootHub>> _rootHubs;

	ProducerRing _cmdRing;
	EventRing _eventRing;

	int _numPorts;
	int _maxDeviceSlots;

	bool _useMsis;

	proto::Enumerator _enumerator;

	bool _largeCtx;

	mbus::Entity _entity;
};


