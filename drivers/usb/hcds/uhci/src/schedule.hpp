
#include <queue>
#include <async/doorbell.hpp>

struct DeviceState;
struct ConfigurationState;
struct InterfaceState;
struct EndpointState;

// ----------------------------------------------------------------------------
// Memory management.
// ----------------------------------------------------------------------------

template<typename T>
struct contiguous_delete {
	void operator() (T *pointer) {
		contiguousAllocator.free(pointer);
	}
};

template<typename T>
struct contiguous_delete<T[]> {
	void operator() (T *pointer);
};

template<typename T>
using contiguous_ptr = std::unique_ptr<T, contiguous_delete<T>>;

template<typename T, typename... Args>
contiguous_ptr<T> make_contiguous(Args &&... args);

// Base class for classes that represent elements of the UHCI schedule.
// All those classes are linked into a list that represents a part of the schedule.
// TODO: They need to be freed through a reclaim mechansim.
struct ScheduleItem : boost::intrusive::list_base_hook<> {
	ScheduleItem()
	: reclaimFrame(-1) { }

	virtual ~ScheduleItem() {
		assert(reclaimFrame != -1);
	}

	int64_t reclaimFrame;
};

struct Transaction : ScheduleItem {
	Transaction();
	
	async::result<void> future();

	void setupTransfers(TransferDescriptor *transfers, size_t num_transfers);
	QueueHead::LinkPointer head();
	void dumpTransfer();
	bool progress();

	async::promise<void> promise;
	size_t numTransfers;
	TransferDescriptor *transfers;
	size_t numComplete;
};


struct ControlTransaction : Transaction {
	ControlTransaction(SetupPacket setup, void *buffer, int address,
			int endpoint, size_t packet_size, XferFlags flags);

private:
	SetupPacket _setup;
};


struct NormalTransaction : Transaction {
	NormalTransaction(void *buffer, size_t length, int address,
			int endpoint, size_t packet_size, XferFlags flags);
};


struct ScheduleEntity {
	virtual	QueueHead::LinkPointer head() = 0;
	virtual void linkNext(QueueHead::LinkPointer link) = 0;
	virtual void progress() = 0;

	boost::intrusive::list_member_hook<> scheduleHook;
};


struct DummyEntity : ScheduleEntity {
	DummyEntity();

	QueueHead::LinkPointer head() override;
	void linkNext(QueueHead::LinkPointer link) override;
	void progress() override;

	TransferDescriptor *_transfer;
	
	boost::intrusive::list<Transaction> transactions;
};

struct QueueEntity : ScheduleItem {
	QueueEntity();

	contiguous_ptr<QueueHead> head;

	boost::intrusive::list<Transaction> transactions;
};

// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

struct Controller : std::enable_shared_from_this<Controller> {
	friend struct ConfigurationState;

	Controller(uint16_t base, helix::UniqueIrq irq);

	void initialize();
	async::result<void> pollDevices();
	async::result<void> probeDevice();
	void activatePeriodic(int frame, ScheduleEntity *entity);
	cofiber::no_future handleIrqs();

private:
	uint16_t _base;
	helix::UniqueIrq _irq;

	DummyEntity _irqDummy;

	uint16_t _lastFrame;
	int64_t _frameCounter;
	async::doorbell _pollDoorbell;

	void _updateFrame();

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

public:
	async::result<std::string> configurationDescriptor(int address);
	async::result<void> useConfiguration(int address, int configuration);
	async::result<void> useInterface(int address, int interface, int alternative);

	// ------------------------------------------------------------------------
	// Transfer functions.
	// ------------------------------------------------------------------------
public:
	async::result<void> transfer(int address, int pipe, ControlTransfer info);
	async::result<void> transfer(int address, PipeType type, int pipe, InterruptTransfer info);
	async::result<void> transfer(int address, PipeType type, int pipe, BulkTransfer info);

private:
	async::result<void> _directTransfer(int address, int endpoint, ControlTransfer info,
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

	boost::intrusive::list<
		ScheduleEntity,
		boost::intrusive::member_hook<
			ScheduleEntity,
			boost::intrusive::list_member_hook<>,
			&ScheduleEntity::scheduleHook
		>
	> _interruptSchedule[1024];

	boost::intrusive::list<QueueEntity> _asyncSchedule;
	
	boost::intrusive::list<ScheduleItem> _reclaimQueue;

	QueueHead _periodicQh[1024];
	QueueHead _asyncQh;
};

// ----------------------------------------------------------------------------
// DeviceState
// ----------------------------------------------------------------------------

struct DeviceState : DeviceData {
	explicit DeviceState(std::shared_ptr<Controller> controller, int device);

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
	async::result<void> transfer(InterruptTransfer info) override;
	async::result<void> transfer(BulkTransfer info) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	PipeType _type;
	int _endpoint;
};

