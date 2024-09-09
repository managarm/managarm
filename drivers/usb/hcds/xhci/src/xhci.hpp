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

struct Controller;

// ----------------------------------------------------------------
// Interrupter
// ----------------------------------------------------------------

struct Interrupter {
	Interrupter(EventRing *ring, arch::mem_space space)
	: _ring{ring}, _space{space} { }

	void initialize();
	async::detached handleIrqs(helix::UniqueIrq &irq);

private:
	bool _isBusy();
	void _clearPending();
	void _updateDequeue();

	EventRing *_ring;
	arch::mem_space _space;
};

// ----------------------------------------------------------------
// Device & {Configuration,Interface,Endpoint}State
// ----------------------------------------------------------------

inline int getEndpointIndex(int endpoint, proto::PipeType dir) {
	using proto::PipeType;

	// For control endpoints the index is:
	//  DCI = (Endpoint Number * 2) + 1.
	// For interrupt, bulk, isoch, the index is:
	//  DCI = (Endpoint Number * 2) + Direction,
	//    where Direction = '0' for OUT endpoints
	//    and '1' for IN endpoints.

	return endpoint * 2 +
		((dir == PipeType::in || dir == PipeType::control)
				? 1 : 0);
}

struct EndpointState;

struct Device final : proto::DeviceData, std::enable_shared_from_this<Device> {
	Device(Controller *controller);

	// Public API inherited from DeviceData.
	arch::dma_pool *setupPool() override;
	arch::dma_pool *bufferPool() override;

	async::result<frg::expected<proto::UsbError, std::string>>
	deviceDescriptor() override;

	async::result<frg::expected<proto::UsbError, std::string>>
	configurationDescriptor(uint8_t configuration) override;

	async::result<frg::expected<proto::UsbError, proto::Configuration>>
	useConfiguration(uint8_t index, uint8_t value) override;

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(proto::ControlTransfer info) override;


	void submit(int endpoint);

	async::result<frg::expected<proto::UsbError>>
	enumerate(size_t rootPort, size_t port, uint32_t route, std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed, int slotType);

	async::result<frg::expected<proto::UsbError>>
	readDescriptor(arch::dma_buffer_view dest, uint16_t desc);

	async::result<frg::expected<proto::UsbError>>
	setupEndpoint(int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type);

	async::result<frg::expected<proto::UsbError>>
	configureHub(std::shared_ptr<proto::Hub> hub, proto::DeviceSpeed speed);

	size_t slot() const {
		return _slotId;
	}

	Controller *controller() const {
		return _controller;
	}

	std::shared_ptr<EndpointState> endpoint(int endpointId) {
		return _endpoints[endpointId - 1];
	}

private:
	int _slotId;

	Controller *_controller;

	DeviceContext _devCtx;

	void _initEpCtx(InputContext &ctx, int endpoint, proto::PipeType dir, size_t maxPacketSize, proto::EndpointType type);

	std::array<std::shared_ptr<EndpointState>, 31> _endpoints;
};


struct EndpointState final : proto::EndpointData {
	explicit EndpointState(Device *device, int endpointId, proto::EndpointType type, size_t maxPacketSize)
	: _device{device}, _endpointId{endpointId}, _type{type},
		_maxPacketSize{maxPacketSize}, _transferRing{device->controller()} { }

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(proto::ControlTransfer info) override;

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(proto::InterruptTransfer info) override;

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(proto::BulkTransfer info) override;

	ProducerRing &transferRing() {
		return _transferRing;
	}

private:
	Device *_device;
	int _endpointId;
	proto::EndpointType _type;

	size_t _maxPacketSize;
	ProducerRing _transferRing;

	async::result<frg::expected<proto::UsbError, size_t>>
	_bulkOrInterruptXfer(arch::dma_buffer_view buffer);

	async::result<frg::expected<proto::UsbError>>
	_resetAfterError(size_t nextDequeue, bool nextCycle);
};


struct ConfigurationState final : proto::ConfigurationData {
	explicit ConfigurationState(std::shared_ptr<Device> device)
	: _device{device} { }

	async::result<frg::expected<proto::UsbError, proto::Interface>>
	useInterface(int number, int alternative) override;

private:
	std::shared_ptr<Device> _device;
};


struct InterfaceState final : proto::InterfaceData {
	explicit InterfaceState(std::shared_ptr<Device> device, int interface)
	: proto::InterfaceData{interface}, _device{device} { }

	async::result<frg::expected<proto::UsbError, proto::Endpoint>>
	getEndpoint(proto::PipeType type, int number) override {
		co_return proto::Endpoint{_device->endpoint(getEndpointIndex(number, type))};
	}

private:
	std::shared_ptr<Device> _device;
};

// ----------------------------------------------------------------
// Controller
// ----------------------------------------------------------------

struct Controller final : proto::BaseController {
	Controller(protocols::hw::Device hw_device,
			mbus_ng::Entity entity,
			helix::Mapping mapping,
			helix::UniqueDescriptor mmio,
			helix::UniqueIrq irq,
			std::string name);

	virtual ~Controller() = default;

	async::detached initialize();

	async::result<void> enumerateDevice(std::shared_ptr<proto::Hub> hub, int port, proto::DeviceSpeed speed) override;

	arch::os::contiguous_pool *memoryPool() {
		return &_memoryPool;
	}

	void processEvent(Event ev);

	void ringDoorbell(uint8_t doorbell, uint8_t target, uint16_t streamId = 0);

	async::result<Event> submitCommand(RawTrb trb) {
		ProducerRing::Transaction tx;
		_cmdRing.pushRawTrb(trb, &tx);

		ringDoorbell(0, 0);

		co_return co_await tx.command();
	}

	bool largeCtx() const {
		return _largeCtx;
	}

	void setDeviceContext(size_t slot, DeviceContext &ctx) {
		_dcbaa[slot] = helix::ptrToPhysical(ctx.rawData());
	}

	std::string_view name() const {
		return _name;
	}

	friend std::ostream &operator<<(std::ostream &os, Controller *controller) {
		return os << "xhci " << (controller ? controller->name() : "(null)") << ": ";
	}

	friend std::ostream &operator<<(std::ostream &os, Controller &controller) {
		return os << &controller;
	}

private:
	struct SupportedProtocol;

	struct Port {
		Port(int id, arch::mem_space space, Controller *controller, SupportedProtocol *port);
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
		async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> issueReset();

	private:
		uint8_t getLinkStatus();
		uint8_t getSpeed();
		int _id;
		Controller *_controller;
		std::shared_ptr<Device> _device;
		SupportedProtocol *_proto;
		arch::mem_space _space;

		async::sequenced_event _pollEv;
		uint64_t _pollSeq = 0;
		proto::PortState _state{};
	};

	struct RootHub final : proto::Hub {
		RootHub(Controller *controller, SupportedProtocol &proto, arch::mem_space portSpace, mbus_ng::EntityManager entity);

		size_t numPorts() override;
		async::result<proto::PortState> pollState(int port) override;
		async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> issueReset(int port) override;

		SupportedProtocol *protocol() {
			return _proto;
		}

		auto entityId() {
			return _entity.id();
		}

	private:
		Controller *_controller;
		SupportedProtocol *_proto;
		std::vector<std::unique_ptr<Port>> _ports;
		mbus_ng::EntityManager _entity;
	};

	struct SupportedProtocol {
		int minor;
		int major;

		size_t compatiblePortStart;
		size_t compatiblePortCount;

		size_t slotType;
	};

	std::vector<SupportedProtocol> _supportedProtocols;

	protocols::hw::Device _hw_device;
	helix::Mapping _mapping;
	helix::UniqueDescriptor _mmio;
	helix::UniqueIrq _irq;
	arch::mem_space _space;
	arch::mem_space _doorbells;

	std::string _name;

	void _processExtendedCapabilities();

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

	proto::Enumerator _enumerator;

	bool _largeCtx;

	mbus_ng::Entity _entity;
};
