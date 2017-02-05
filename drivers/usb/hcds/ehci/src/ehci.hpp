
#include <queue>

#include <arch/mem_space.hpp>
#include <async/result.hpp>
#include <async/doorbell.hpp>

#include "spec.hpp"

// ----------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------

struct Controller : std::enable_shared_from_this<Controller> {
	Controller(void *address, helix::UniqueIrq irq);
	
	void initialize();
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
		QueueEntity(arch::dma_object<QueueHead> the_head);

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
	// Transfer functions.
	// ------------------------------------------------------------------------

public:
	async::result<void> transfer(int address, int pipe, ControlTransfer info);

private:
	static Transaction *_buildControl(int address, int pipe, XferFlags dir,
			arch::dma_object_view<SetupPacket> setup, arch::dma_buffer_view buffer,
			size_t max_packet_size);
	async::result<void> _directTransfer(int address, int pipe, ControlTransfer info,
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
	arch::mem_space _space; 
	helix::UniqueIrq _irq;
	arch::mem_space _operational;

	int _numPorts;
	async::doorbell _pollDoorbell;
};

