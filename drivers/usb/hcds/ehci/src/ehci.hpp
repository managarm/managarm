
#include <queue>

#include <arch/mem_space.hpp>
#include <async/doorbell.hpp>
#include <async/mutex.hpp>
#include <async/result.hpp>
#include <helix/memory.hpp>

#include "spec.hpp"

struct Controller;
struct DeviceState;
struct ConfigurationState;
struct InterfaceState;
struct EndpointState;

// TODO: This could be moved to a "USB core" driver.
struct Enumerator {
	Enumerator(Controller *controller);

	// Called by the USB hub driver once a device connects to a port.
	void connectPort(int port);

	// Called by the USB hub driver once a device completes reset.
	void enablePort(int port);

private:
	enum class State {
		null, resetting, probing
	};

	cofiber::no_future _reset();
	cofiber::no_future _probe();

	Controller *_controller;
	State _state;
	int _activePort;
	async::mutex _addressMutex;
};

// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

struct Controller : std::enable_shared_from_this<Controller> {
	Controller(protocols::hw::Device hw_device,
			helix::Mapping mapping,
			helix::UniqueDescriptor mmio, helix::UniqueIrq irq);
	
	cofiber::no_future initialize();
	async::result<void> probeDevice();
	cofiber::no_future handleIrqs();
	
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
		async::promise<size_t> promise;
		async::promise<void> voidPromise;
	};

	struct QueueEntity : AsyncItem {
		QueueEntity(arch::dma_object<QueueHead> the_head, int address,
				int pipe, PipeType type, size_t packet_size);

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

public:
	async::result<std::string> configurationDescriptor(int address);
	async::result<void> useConfiguration(int address, int configuration);
	async::result<void> useInterface(int address, int interface, int alternative);

	// ------------------------------------------------------------------------
	// Transfer functions.
	// ------------------------------------------------------------------------
	
	static Transaction *_buildControl(XferFlags dir,
			arch::dma_object_view<SetupPacket> setup, arch::dma_buffer_view buffer,
			size_t max_packet_size);
	static Transaction *_buildInterruptOrBulk(XferFlags dir,
			arch::dma_buffer_view buffer, size_t max_packet_size,
			bool lazy_notification);


	async::result<void> transfer(int address, int pipe, ControlTransfer info);
	async::result<size_t> transfer(int address, PipeType type, int pipe, InterruptTransfer info);
	async::result<size_t> transfer(int address, PipeType type, int pipe, BulkTransfer info);

private:
	async::result<void> _directTransfer(ControlTransfer info,
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
	cofiber::no_future resetPort(int number);

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
	Enumerator _enumerator;
};

// ----------------------------------------------------------------------------
// DeviceState
// ----------------------------------------------------------------------------

struct DeviceState final : DeviceData {
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

struct ConfigurationState final : ConfigurationData {
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

struct InterfaceState final : InterfaceData {
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

struct EndpointState final : EndpointData {
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

