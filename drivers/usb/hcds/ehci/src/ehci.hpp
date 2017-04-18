
#include <queue>

#include <arch/mem_space.hpp>
#include <async/result.hpp>
#include <async/doorbell.hpp>

#include "spec.hpp"

struct DeviceState;
struct ConfigurationState;
struct InterfaceState;
struct EndpointState;

// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

struct Controller : std::enable_shared_from_this<Controller> {
	Controller(protocols::hw::Device hw_device, void *address, helix::UniqueIrq irq);
	
	cofiber::no_future initialize();
	async::result<void> pollDevices();	
	async::result<void> probeDevice();
	cofiber::no_future handleIrqs();
	
	// ------------------------------------------------------------------------
	// Schedule classes.
	// ------------------------------------------------------------------------

	struct AsyncItem : boost::intrusive::list_base_hook<> {

	};

	struct Transaction : AsyncItem {
		explicit Transaction(arch::dma_array<TransferDescriptor> transfers)
		: transfers{std::move(transfers)}, numComplete{0} { }
		
		arch::dma_array<TransferDescriptor> transfers;
		size_t numComplete;
		async::promise<void> promise;
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
			arch::dma_buffer_view buffer, size_t max_packet_size);


	async::result<void> transfer(int address, int pipe, ControlTransfer info);
	async::result<void> transfer(int address, PipeType type, int pipe, InterruptTransfer info);
	async::result<void> transfer(int address, PipeType type, int pipe, BulkTransfer info);

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
	// Debugging functions.
	// ----------------------------------------------------------------------------
	
	void _dump(Transaction *transaction);
	void _dump(QueueEntity *entity);
	

private:
	protocols::hw::Device _hwDevice;
	arch::mem_space _space; 
	helix::UniqueIrq _irq;
	arch::mem_space _operational;

	int _numPorts;
	async::doorbell _pollDoorbell;
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
	async::result<void> transfer(InterruptTransfer info) override;
	async::result<void> transfer(BulkTransfer info) override;

private:
	std::shared_ptr<Controller> _controller;
	int _device;
	PipeType _type;
	int _endpoint;
};

