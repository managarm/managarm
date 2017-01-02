
#include <queue>

struct DeviceState;
struct ConfigurationState;
struct InterfaceState;
struct EndpointState;

struct QueuedTransaction {
	QueuedTransaction();
	
	async::result<void> future();

	void setupTransfers(TransferDescriptor *transfers, size_t num_transfers);
	QueueHead::LinkPointer head();
	void dumpTransfer();
	bool progress();

	boost::intrusive::list_member_hook<> transactionHook;

private:
	async::promise<void> _promise;
	size_t _numTransfers;
	TransferDescriptor *_transfers;
	size_t _completeCounter;
};


struct ControlTransaction : QueuedTransaction {
	ControlTransaction(SetupPacket setup, void *buffer, int address,
			int endpoint, size_t packet_size, XferFlags flags);

private:
	SetupPacket _setup;
};


struct NormalTransaction : QueuedTransaction {
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
	
	boost::intrusive::list<
		QueuedTransaction,
		boost::intrusive::member_hook<
			QueuedTransaction,
			boost::intrusive::list_member_hook<>,
			&QueuedTransaction::transactionHook
		>
	> transactionList;
};

struct QueueEntity : ScheduleEntity {
	QueueEntity();

	QueueHead::LinkPointer head() override;
	void linkNext(QueueHead::LinkPointer link) override;
	void progress() override;

	QueueHead *_queue;
	
	boost::intrusive::list<
		QueuedTransaction,
		boost::intrusive::member_hook<
			QueuedTransaction,
			boost::intrusive::list_member_hook<>,
			&QueuedTransaction::transactionHook
		>
	> transactionList;
};

// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

struct Controller : std::enable_shared_from_this<Controller> {
	Controller(uint16_t base, helix::UniqueIrq irq);

	void initialize();
	async::result<void> pollDevices();
	async::result<void> probeDevice();
	void activateAsync(ScheduleEntity *entity);
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

	QueueHead _periodicQh[1024];
	QueueHead _asyncQh;

	DummyEntity _irqDummy;

	uint16_t _lastFrame;
	uint64_t _lastCounter;

	std::queue<int> _addressStack;
	std::shared_ptr<DeviceState> _activeDevices[128];
};

// ----------------------------------------------------------------------------
// DeviceState
// ----------------------------------------------------------------------------

struct DeviceState : DeviceData, std::enable_shared_from_this<DeviceState> {
	async::result<std::string> configurationDescriptor() override;
	async::result<Configuration> useConfiguration(int number) override;
	async::result<void> transfer(ControlTransfer info) override;
	
	uint8_t address;
	std::shared_ptr<EndpointState> controlStates[16];
	std::shared_ptr<EndpointState> outStates[16];
	std::shared_ptr<EndpointState> inStates[16];
	std::shared_ptr<Controller> _controller;
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

