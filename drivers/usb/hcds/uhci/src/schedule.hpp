
#include <queue>
#include <arch/dma_pool.hpp>
#include <arch/io_space.hpp>
#include <async/recurring-event.hpp>
#include <async/mutex.hpp>
#include <protocols/hw/client.hpp>

struct DeviceState;
struct ConfigurationState;
struct InterfaceState;
struct EndpointState;

// ----------------------------------------------------------------------------
// Various stuff that needs to be moved to some USB core library.
// ----------------------------------------------------------------------------

struct BaseController {
protected:
	~BaseController() = default;

public:
	virtual async::result<void> enumerateDevice(bool low_speed) = 0;
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
protected:
	~Hub() = default;

public:
	virtual size_t numPorts() = 0;
	virtual async::result<PortState> pollState(int port) = 0;
	virtual async::result<frg::expected<UsbError, bool>> issueReset(int port, bool *low_speed) = 0;
};

struct Enumerator {
	Enumerator(BaseController *controller)
	: _controller{controller} { }

	void observeHub(std::shared_ptr<Hub> hub);

private:
	async::detached _observePort(std::shared_ptr<Hub> hub, int port);
	async::result<void> _observationCycle(Hub *hub, int port);

	BaseController *_controller;
	async::mutex _enumerateMutex;
};

// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

struct Controller final : std::enable_shared_from_this<Controller>, BaseController {
	friend struct ConfigurationState;

	struct RootHub final : Hub {
		RootHub(Controller *controller)
		: _controller{controller} { }

		size_t numPorts() override;
		async::result<PortState> pollState(int port) override;
		async::result<frg::expected<UsbError, bool>> issueReset(int port, bool *low_speed) override;

	private:
		Controller *_controller;
	};

	Controller(protocols::hw::Device hw_device, uintptr_t base,
			arch::io_space space, helix::UniqueIrq irq);

	void initialize();
	async::detached _handleIrqs();
	async::detached _refreshFrame();
	
	async::result<void> enumerateDevice(bool low_speed) override;

private:
	protocols::hw::Device _hwDevice;
	uintptr_t _ioBase;
	arch::io_space _ioSpace;
	helix::UniqueIrq _irq;

	uint16_t _lastFrame;
	int64_t _frameCounter;
	PortState _portState[2];
	async::recurring_event _portDoorbell;

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
		: transfers{std::move(transfers)}, autoToggle{false},
				numComplete{0}, lengthComplete{0},
				allowShortPackets{allow_short_packets} { }
		
		arch::dma_array<TransferDescriptor> transfers;
		bool autoToggle;
		size_t numComplete;
		size_t lengthComplete;
		bool allowShortPackets;
		async::promise<frg::expected<UsbError, size_t>> promise;
		async::promise<frg::expected<UsbError>> voidPromise;
	};

	struct QueueEntity : ScheduleItem {
		QueueEntity(arch::dma_object<QueueHead> the_head)
		: head{std::move(the_head)}, toggleState{false} {
			head->_linkPointer = QueueHead::LinkPointer();
			head->_elementPointer = QueueHead::ElementPointer();
		}

		arch::dma_object<QueueHead> head;
		bool toggleState;
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
		bool lowSpeed;
	};

	Enumerator _enumerator;
	std::queue<int> _addressStack;
	DeviceSlot _activeDevices[128];

public:
	async::result<frg::expected<UsbError, std::string>> configurationDescriptor(int address);

	async::result<frg::expected<UsbError>>
	useConfiguration(int address, int configuration);

	async::result<frg::expected<UsbError>>
	useInterface(int address, int interface, int alternative);

	// ------------------------------------------------------------------------
	// Transfer functions.
	// ------------------------------------------------------------------------
	
	static Transaction *_buildControl(int address, int pipe, XferFlags dir,
			arch::dma_object_view<SetupPacket> setup, arch::dma_buffer_view buffer,
			bool low_speed, size_t max_packet_size);
	static Transaction *_buildInterruptOrBulk(int address, int pipe, XferFlags dir,
			arch::dma_buffer_view buffer,
			bool low_speed, size_t max_packet_size,
			bool allow_short_packets);

public:
	async::result<frg::expected<UsbError>> transfer(int address, int pipe, ControlTransfer info);

	async::result<frg::expected<UsbError, size_t>>
	transfer(int address, PipeType type, int pipe, InterruptTransfer info);

	async::result<frg::expected<UsbError, size_t>>
	transfer(int address, PipeType type, int pipe, BulkTransfer info);

private:
	async::result<frg::expected<UsbError>>
	_directTransfer(int address, int pipe, ControlTransfer info,
			QueueEntity *queue, bool low_speed, size_t max_packet_size);

private:
	// ------------------------------------------------------------------------
	// Schedule management.
	// ------------------------------------------------------------------------
	
	void _linkInterrupt(QueueEntity *entity, int order, int index);
	void _linkAsync(QueueEntity *entity);
	void _linkIntoScheduleTree(int order, int index, QueueEntity *entity);
	void _linkTransaction(QueueEntity *queue, Transaction *transaction);

	void _progressSchedule();
	void _progressQueue(QueueEntity *entity);

	void _reclaim(ScheduleItem *item);

	boost::intrusive::list<QueueEntity> _interruptSchedule[2 * 1024 - 1];
	boost::intrusive::list<QueueEntity> _asyncSchedule;
	std::vector<QueueEntity *> _activeEntities;

	// This queue holds all schedule structs that are currently
	// being garbage collected.
	boost::intrusive::list<ScheduleItem> _reclaimQueue;

	FrameList *_frameList;
	
	// ----------------------------------------------------------------------------
	// Debugging functions.
	// ----------------------------------------------------------------------------

	static void _dump(Transaction *transaction);
};

// ----------------------------------------------------------------------------
// DeviceState
// ----------------------------------------------------------------------------

struct DeviceState final : DeviceData {
	explicit DeviceState(std::shared_ptr<Controller> controller, int device);

	arch::dma_pool *setupPool() override;
	arch::dma_pool *bufferPool() override;

	async::result<frg::expected<UsbError, std::string>> configurationDescriptor() override;
	async::result<frg::expected<UsbError, Configuration>> useConfiguration(int number) override;
	async::result<frg::expected<UsbError>> transfer(ControlTransfer info) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
};

// ----------------------------------------------------------------------------
// ConfigurationState
// ----------------------------------------------------------------------------

struct ConfigurationState final : ConfigurationData {
	explicit ConfigurationState(std::shared_ptr<Controller> controller,
			int device, int configuration);

	async::result<frg::expected<UsbError, Interface>>
	useInterface(int number, int alternative) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	int _configuration;
};

// ----------------------------------------------------------------------------
// InterfaceState
// ----------------------------------------------------------------------------

struct InterfaceState final : InterfaceData {
	explicit InterfaceState(std::shared_ptr<Controller> controller,
			int device, int configuration);

	async::result<frg::expected<UsbError, Endpoint>>
	getEndpoint(PipeType type, int number) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	int _interface;
};

// ----------------------------------------------------------------------------
// EndpointState
// ----------------------------------------------------------------------------

struct EndpointState final : EndpointData {
	explicit EndpointState(std::shared_ptr<Controller> controller,
			int device, PipeType type, int endpoint);

	async::result<frg::expected<UsbError>> transfer(ControlTransfer info) override;
	async::result<frg::expected<UsbError, size_t>> transfer(InterruptTransfer info) override;
	async::result<frg::expected<UsbError, size_t>> transfer(BulkTransfer info) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	PipeType _type;
	int _endpoint;
};

