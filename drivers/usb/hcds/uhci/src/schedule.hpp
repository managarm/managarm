
#include <queue>
#include <arch/dma_pool.hpp>
#include <arch/io_space.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>

struct DeviceState;
struct ConfigurationState;
struct InterfaceState;
struct EndpointState;

// ----------------------------------------------------------------------------
// Various stuff that needs to be moved to some USB core library.
// ----------------------------------------------------------------------------

struct BaseController {
	virtual async::result<void> enumerateDevice() = 0;
};

namespace hub_status {
	static constexpr uint32_t connect = 0x01;
	static constexpr uint32_t enable = 0x02;
	static constexpr uint32_t reset = 0x04;
}

struct PortState {
	uint32_t status;
	uint32_t changes;
};

struct Hub {
	virtual size_t numPorts() = 0;
	virtual async::result<PortState> pollState(int port) = 0;
	virtual async::result<void> issueReset(int port) = 0;
};

struct Enumerator {
	Enumerator(BaseController *controller)
	: _controller{controller} { }

	void observeHub(std::shared_ptr<Hub> hub);

private:
	cofiber::no_future _observePort(std::shared_ptr<Hub> hub, int port);

	BaseController *_controller;
	async::mutex _enumerateMutex;
};

// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

struct Controller : std::enable_shared_from_this<Controller>, BaseController {
	friend struct ConfigurationState;

	struct RootHub : Hub {
		RootHub(Controller *controller)
		: _controller{controller} { }

		size_t numPorts() override;
		async::result<PortState> pollState(int port) override;
		async::result<void> issueReset(int port) override;

	private:
		Controller *_controller;
	};

	Controller(arch::io_space base, helix::UniqueIrq irq);

	void initialize();
	cofiber::no_future _handleIrqs();
	cofiber::no_future _refreshFrame();
	
	async::result<void> enumerateDevice() override;

private:
	arch::io_space _base;
	helix::UniqueIrq _irq;

	uint16_t _lastFrame;
	int64_t _frameCounter;
	PortState _portState[2];
	async::doorbell _portDoorbell;

	void _updateFrame();

	// ------------------------------------------------------------------------
	// Schedule classes.
	// ------------------------------------------------------------------------

	// Base class for classes that represent elements of the UHCI schedule.
	// All those classes are linked into a list that represents a part of the schedule.
	// They need to be freed through the reclaim mechansim.
	struct ScheduleItem : boost::intrusive::list_base_hook<> {
		ScheduleItem()
		: reclaimFrame(-1) { }

		virtual ~ScheduleItem() {
			assert(reclaimFrame != -1);
		}

		int64_t reclaimFrame;
	};

	struct Transaction : ScheduleItem {
		explicit Transaction(arch::dma_array<TransferDescriptor> transfers,
				bool allow_short_packets = false)
		: transfers{std::move(transfers)}, numComplete{0},
				allowShortPackets{allow_short_packets} { }
		
		arch::dma_array<TransferDescriptor> transfers;
		size_t numComplete;
		bool allowShortPackets;
		async::promise<size_t> promise;
		async::promise<void> voidPromise;
	};

	struct QueueEntity : ScheduleItem {
		QueueEntity(arch::dma_object<QueueHead> the_head)
		: head{std::move(the_head)} {
			head->_linkPointer = QueueHead::LinkPointer();
			head->_elementPointer = QueueHead::ElementPointer();
		}

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

	Enumerator _enumerator;
	std::queue<int> _addressStack;
	DeviceSlot _activeDevices[128];

public:
	async::result<std::string> configurationDescriptor(int address);
	async::result<void> useConfiguration(int address, int configuration);
	async::result<void> useInterface(int address, int interface, int alternative);

	// ------------------------------------------------------------------------
	// Transfer functions.
	// ------------------------------------------------------------------------
	
	static Transaction *_buildControl(int address, int pipe, XferFlags dir,
			arch::dma_object_view<SetupPacket> setup, arch::dma_buffer_view buffer,
			size_t max_packet_size);
	static Transaction *_buildInterruptOrBulk(int address, int pipe, XferFlags dir,
			arch::dma_buffer_view buffer, size_t max_packet_size,
			bool allow_short_packets);

public:
	async::result<void> transfer(int address, int pipe, ControlTransfer info);
	async::result<size_t> transfer(int address, PipeType type, int pipe, InterruptTransfer info);
	async::result<size_t> transfer(int address, PipeType type, int pipe, BulkTransfer info);

private:
	async::result<void> _directTransfer(int address, int pipe, ControlTransfer info,
			QueueEntity *queue, size_t max_packet_size);

private:
	// ------------------------------------------------------------------------
	// Schedule management.
	// ------------------------------------------------------------------------
	
	void _linkInterrupt(QueueEntity *entity);
	void _linkAsync(QueueEntity *entity);
	void _linkTransaction(QueueEntity *queue, Transaction *transaction);

	void _progressSchedule();
	void _progressQueue(QueueEntity *entity);

	void _reclaim(ScheduleItem *item);

	boost::intrusive::list<QueueEntity> _interruptSchedule[1024];

	boost::intrusive::list<QueueEntity> _asyncSchedule;

	// This queue holds all schedule structs that are currently
	// being garbage collected.
	boost::intrusive::list<ScheduleItem> _reclaimQueue;

	arch::dma_array<QueueHead> _periodicQh;
	arch::dma_object<QueueHead> _asyncQh;
	
	// ----------------------------------------------------------------------------
	// Debugging functions.
	// ----------------------------------------------------------------------------

	static void _dump(Transaction *transaction);
};

// ----------------------------------------------------------------------------
// DeviceState
// ----------------------------------------------------------------------------

struct DeviceState : DeviceData {
	explicit DeviceState(std::shared_ptr<Controller> controller, int device);

	arch::dma_pool *setupPool() override;
	arch::dma_pool *bufferPool() override;

	async::result<std::string> configurationDescriptor() override;
	async::result<Configuration> useConfiguration(int number) override;
	async::result<void> transfer(ControlTransfer info) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
};

// ----------------------------------------------------------------------------
// ConfigurationState
// ----------------------------------------------------------------------------

struct ConfigurationState : ConfigurationData {
	explicit ConfigurationState(std::shared_ptr<Controller> controller,
			int device, int configuration);

	async::result<Interface> useInterface(int number, int alternative) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	int _configuration;
};

// ----------------------------------------------------------------------------
// InterfaceState
// ----------------------------------------------------------------------------

struct InterfaceState : InterfaceData {
	explicit InterfaceState(std::shared_ptr<Controller> controller,
			int device, int configuration);

	async::result<Endpoint> getEndpoint(PipeType type, int number) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	int _interface;
};

// ----------------------------------------------------------------------------
// EndpointState
// ----------------------------------------------------------------------------

struct EndpointState : EndpointData {
	explicit EndpointState(std::shared_ptr<Controller> controller,
			int device, PipeType type, int endpoint);

	async::result<void> transfer(ControlTransfer info) override;
	async::result<size_t> transfer(InterruptTransfer info) override;
	async::result<size_t> transfer(BulkTransfer info) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	PipeType _type;
	int _endpoint;
};

