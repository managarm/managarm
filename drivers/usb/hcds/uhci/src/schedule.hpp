
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
	async::result<void> transfer(std::shared_ptr<DeviceState> device_state,
			int endpoint,  ControlTransfer info);
	async::result<void> transfer(std::shared_ptr<DeviceState> device_state,
			int endpoint, XferFlags flags, InterruptTransfer info);
	async::result<void> transfer(std::shared_ptr<DeviceState> device_state,
			int endpoint, XferFlags flags, BulkTransfer info);
	cofiber::no_future handleIrqs();

private:
	uint16_t _base;
	helix::UniqueIrq _irq;

	DummyEntity _irqDummy;

	uint16_t _lastFrame;
	int64_t _frameCounter;
	async::doorbell _pollDoorbell;

	void _updateFrame();

	std::queue<int> _addressStack;
	std::shared_ptr<DeviceState> _activeDevices[128];

	// ------------------------------------------------------------------------
	// Schedule management.
	// ------------------------------------------------------------------------
	
	void _linkInterrupt(QueueEntity *entity);
	void _linkAsync(QueueEntity *entity);

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

struct DeviceState : DeviceData, std::enable_shared_from_this<DeviceState> {
	explicit DeviceState(std::shared_ptr<Controller> controller);

	async::result<std::string> configurationDescriptor() override;
	async::result<Configuration> useConfiguration(int number) override;
	async::result<void> transfer(ControlTransfer info) override;
	
	std::shared_ptr<Controller> _controller;

	uint8_t address;

	std::shared_ptr<EndpointState> controlStates[16];
	std::shared_ptr<EndpointState> outStates[16];
	std::shared_ptr<EndpointState> inStates[16];
};

// ----------------------------------------------------------------------------
// ConfigurationState
// ----------------------------------------------------------------------------

struct ConfigurationState : ConfigurationData, std::enable_shared_from_this<ConfigurationState> {
	ConfigurationState(std::shared_ptr<DeviceState> device);

	async::result<Interface> useInterface(int number, int alternative) override;

//private:
	std::shared_ptr<DeviceState> _device;
};

// ----------------------------------------------------------------------------
// InterfaceState
// ----------------------------------------------------------------------------

struct InterfaceState : InterfaceData {
	InterfaceState(std::shared_ptr<ConfigurationState> config);

	async::result<Endpoint> getEndpoint(PipeType type, int number) override;

//private:
	std::shared_ptr<ConfigurationState> _config;
};

// ----------------------------------------------------------------------------
// EndpointState
// ----------------------------------------------------------------------------

struct EndpointState : EndpointData {
	EndpointState(PipeType type, int number);

	async::result<void> transfer(ControlTransfer info) override;
	async::result<void> transfer(InterruptTransfer info) override;
	async::result<void> transfer(BulkTransfer info) override;

	size_t maxPacketSize;
	std::unique_ptr<QueueEntity> queue;

	std::shared_ptr<InterfaceState> _interface;
	PipeType _type;
	int _number;
};

