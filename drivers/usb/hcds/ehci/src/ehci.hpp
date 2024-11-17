
#include <queue>

#include <arch/mem_space.hpp>
#include <arch/dma_structs.hpp>
#include <async/sequenced-event.hpp>
#include <async/promise.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <helix/memory.hpp>
#include <frg/expected.hpp>
#include <frg/std_compat.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/hub.hpp>

#include "spec.hpp"

namespace proto = protocols::usb;

struct Controller;
struct DeviceState;
struct ConfigurationState;
struct InterfaceState;
struct EndpointState;

// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

struct Controller final : proto::BaseController, std::enable_shared_from_this<Controller> {
	Controller(protocols::hw::Device hw_device,
			mbus_ng::EntityManager entity,
			helix::Mapping mapping,
			helix::UniqueDescriptor mmio, helix::UniqueIrq irq);

	async::detached initialize();
	async::detached handleIrqs();
	async::result<frg::expected<proto::UsbError>>
	enumerateDevice(std::shared_ptr<proto::Hub> hub, int port, proto::DeviceSpeed speed) override;

	// ------------------------------------------------------------------------
	// Schedule classes.
	// ------------------------------------------------------------------------

	struct AsyncItem : boost::intrusive::list_base_hook<> {

	};

	struct Transaction : AsyncItem {
		explicit Transaction(arch::dma_array<TransferDescriptor> transfers, size_t size)
		: transfers{std::move(transfers)}, fullSize{size},
				numComplete{0}, lostSize{0} { }

		arch::dma_array<TransferDescriptor> transfers;
		size_t fullSize;
		size_t numComplete;
		size_t lostSize; // Size lost in short packets.
		async::promise<frg::expected<proto::UsbError, size_t>, frg::stl_allocator> promise;
	};

	struct QueueEntity : AsyncItem {
		QueueEntity(arch::dma_object<QueueHead> the_head, int address,
				int pipe, proto::PipeType type, size_t packet_size);

		bool getReclaim();
		void setReclaim(bool reclaim);
		void setAddress(int address);
		arch::dma_object<QueueHead> head;
		boost::intrusive::list<Transaction> transactions;
	};


	// ------------------------------------------------------------------------
	// Device management.
	// ------------------------------------------------------------------------

	struct EndpointSlot {
		size_t maxPacketSize;
		QueueEntity *queueEntity;
	};

	struct DeviceSlot {
		EndpointSlot controlStates[16];
		EndpointSlot outStates[16];
		EndpointSlot inStates[16];
	};

	std::queue<int> _addressStack;
	DeviceSlot _activeDevices[128];

	// ------------------------------------------------------------------------
	// Root hub.
	// ------------------------------------------------------------------------

	struct Port {
		async::result<proto::PortState> pollState() {
			pollSeq = co_await pollEv.async_wait(pollSeq);
			co_return state;
		}

		async::sequenced_event pollEv;
		uint64_t pollSeq = 0;
		proto::PortState state{};
	};

	struct RootHub final : proto::Hub {
		RootHub(Controller *controller);

		size_t numPorts() override;
		async::result<proto::PortState> pollState(int port) override;
		async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>> issueReset(int port) override;

		Port &port(int portnr) {
			assert(portnr < _controller->_numPorts);
			return *_ports[portnr];
		}

	private:
		Controller *_controller;
		std::vector<std::unique_ptr<Port>> _ports;
	};

	std::shared_ptr<RootHub> _rootHub;

public:
	async::result<frg::expected<proto::UsbError, std::string>> deviceDescriptor(int address);
	async::result<frg::expected<proto::UsbError, std::string>> configurationDescriptor(int address, uint8_t configuration);

	async::result<frg::expected<proto::UsbError>>
	useConfiguration(int address, int configuration);

	async::result<frg::expected<proto::UsbError>>
	useInterface(int address, uint8_t configIndex, uint8_t configValue, int interface, int alternative);

	// ------------------------------------------------------------------------
	// Transfer functions.
	// ------------------------------------------------------------------------

	static Transaction *_buildControl(proto::XferFlags dir,
			arch::dma_object_view<proto::SetupPacket> setup, arch::dma_buffer_view buffer,
			size_t max_packet_size);
	static Transaction *_buildInterruptOrBulk(proto::XferFlags dir,
			arch::dma_buffer_view buffer, size_t max_packet_size,
			bool lazy_notification);


	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(int address, int pipe, proto::ControlTransfer info);

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(int address, proto::PipeType type, int pipe, proto::InterruptTransfer info);

	async::result<frg::expected<proto::UsbError, size_t>>
	transfer(int address, proto::PipeType type, int pipe, proto::BulkTransfer info);

private:
	async::result<frg::expected<proto::UsbError, size_t>> _directTransfer(proto::ControlTransfer info,
			QueueEntity *queue, size_t max_packet_size);


	// ------------------------------------------------------------------------
	// Schedule management.
	// ------------------------------------------------------------------------

	void _linkAsync(QueueEntity *entity);
	void _linkTransaction(QueueEntity *queue, Transaction *transaction);

	void _progressSchedule();
	void _progressQueue(QueueEntity *entity);

	boost::intrusive::list<QueueEntity> _asyncSchedule;
	arch::dma_object<QueueHead> _asyncQh;

	// ----------------------------------------------------------------------------
	// Port management.
	// ----------------------------------------------------------------------------

	void _checkPorts();

public:
	async::result<frg::expected<proto::UsbError, proto::DeviceSpeed>>
	resetPort(int number);

	// ----------------------------------------------------------------------------
	// Debugging functions.
	// ----------------------------------------------------------------------------
private:
	void _dump(Transaction *transaction);
	void _dump(QueueEntity *entity);


private:
	protocols::hw::Device _hwDevice;
	helix::Mapping _mapping;
	helix::UniqueDescriptor _mmio;
	helix::UniqueIrq _irq;
	arch::mem_space _space;
	arch::mem_space _operational;

	int _numPorts;
	proto::Enumerator _enumerator;

	mbus_ng::EntityManager _entity;
};

// ----------------------------------------------------------------------------
// DeviceState
// ----------------------------------------------------------------------------

struct DeviceState final : proto::DeviceData {
	explicit DeviceState(std::shared_ptr<Controller> controller, int device);

	arch::dma_pool *setupPool() override;
	arch::dma_pool *bufferPool() override;

	async::result<frg::expected<proto::UsbError, std::string>> deviceDescriptor() override;
	async::result<frg::expected<proto::UsbError, std::string>> configurationDescriptor(uint8_t configuration) override;
	async::result<frg::expected<proto::UsbError, proto::Configuration>> useConfiguration(uint8_t index, uint8_t value) override;
	async::result<frg::expected<proto::UsbError, size_t>> transfer(proto::ControlTransfer info) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
};

// ----------------------------------------------------------------------------
// ConfigurationState
// ----------------------------------------------------------------------------

struct ConfigurationState final : proto::ConfigurationData {
	explicit ConfigurationState(std::shared_ptr<Controller> controller,
			int device, uint8_t index, uint8_t value);

	async::result<frg::expected<proto::UsbError, proto::Interface>>
	useInterface(int number, int alternative) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	uint8_t _index;
	uint8_t _value;
};

// ----------------------------------------------------------------------------
// InterfaceState
// ----------------------------------------------------------------------------

struct InterfaceState final : proto::InterfaceData {
	explicit InterfaceState(std::shared_ptr<Controller> controller,
			int device, int configuration);

	async::result<frg::expected<proto::UsbError, proto::Endpoint>>
	getEndpoint(proto::PipeType type, int number) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	int _interface [[maybe_unused]];
};

// ----------------------------------------------------------------------------
// EndpointState
// ----------------------------------------------------------------------------

struct EndpointState final : proto::EndpointData {
	explicit EndpointState(std::shared_ptr<Controller> controller,
			int device, proto::PipeType type, int endpoint);

	async::result<frg::expected<proto::UsbError, size_t>> transfer(proto::ControlTransfer info) override;
	async::result<frg::expected<proto::UsbError, size_t>> transfer(proto::InterruptTransfer info) override;
	async::result<frg::expected<proto::UsbError, size_t>> transfer(proto::BulkTransfer info) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	proto::PipeType _type;
	int _endpoint;
};

