
#include <queue>

#include <arch/mem_space.hpp>
#include <async/result.hpp>
#include <async/doorbell.hpp>

struct Controller : std::enable_shared_from_this<Controller> {
	Controller(void *address, helix::UniqueIrq irq);
	
	void initialize();
	async::result<void> pollDevices();

private:
	arch::mem_space _space; 
	helix::UniqueIrq _irq;
	arch::mem_space _operational;

	int _numPorts;
	async::doorbell _pollDoorbell;
};

