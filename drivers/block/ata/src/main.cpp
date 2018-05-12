
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <iostream>
#include <queue>

#include <async/result.hpp>
#include <arch/io_space.hpp>
#include <arch/register.hpp>
#include <helix/ipc.hpp>

#include <blockfs.hpp>

// --------------------------------------------------------
// Controller class
// --------------------------------------------------------

namespace regs {
	inline constexpr arch::scalar_register<uint16_t> inData{0};
	inline constexpr arch::scalar_register<uint8_t> inStatus{7};

	inline constexpr arch::scalar_register<uint8_t> outSectorCount{2};
	inline constexpr arch::scalar_register<uint8_t> outLba1{3};
	inline constexpr arch::scalar_register<uint8_t> outLba2{4};
	inline constexpr arch::scalar_register<uint8_t> outLba3{5};
	inline constexpr arch::scalar_register<uint8_t> outDevice{6};
	inline constexpr arch::scalar_register<uint8_t> outCommand{7};
}

class Controller : public blockfs::BlockDevice {
public:
	Controller();

public:
	void run();

private:
	cofiber::no_future _handleIrqs();

public:
	async::result<void> readSectors(uint64_t sector, void *buffer,
			size_t num_sectors) override;

private:
	void _performRequest();

private:
	enum Commands {
		kCommandReadSectorsExt = 0x24
	};

	enum Flags {
		kStatusErr = 0x01,
		kStatusDrq = 0x08,
		kStatusDf = 0x20,
		kStatusRdy = 0x60,
		kStatusBsy = 0x80,

		kDeviceSlave = 0x10,
		kDeviceLba = 0x40
	};

	struct Request {
		uint64_t sector;
		size_t numSectors;
		size_t sectorsRead;
		void *buffer;
		async::promise<void> promise;
	};

	std::queue<Request> _requestQueue;

	helix::UniqueDescriptor _irq;
	HelHandle _ioHandle;
	arch::io_space _ioSpace;

	bool _inRequest;
};

Controller::Controller()
: BlockDevice{512}, _ioSpace{0x1F0}, _inRequest{false} {
	HelHandle irq_handle;
	HEL_CHECK(helAccessIrq(14, &irq_handle));
	_irq = helix::UniqueDescriptor{irq_handle};

	uintptr_t ports[] = { 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6 };
	HEL_CHECK(helAccessIo(ports, 9, &_ioHandle));
	HEL_CHECK(helEnableIo(_ioHandle));
}

void Controller::run() {
	_handleIrqs();

	blockfs::runDevice(this);
}

COFIBER_ROUTINE(cofiber::no_future, Controller::_handleIrqs(), ([=] {
	uint64_t sequence = 0;
	while(true) {
//		std::cout << "block-ata: Awaiting IRQ." << std::endl;
		helix::AwaitEvent await_irq;
		auto &&submit = helix::submitAwaitEvent(_irq, &await_irq, sequence,
				helix::Dispatcher::global());
		COFIBER_AWAIT submit.async_wait();
		HEL_CHECK(await_irq.error());
		sequence = await_irq.sequence();
//		std::cout << "block-ata: IRQ fired." << std::endl;

		assert(_inRequest);
		assert(!_requestQueue.empty());
		auto request = &_requestQueue.front();
		
		// acknowledge the interrupt
		auto status = _ioSpace.load(regs::inStatus);
		assert(!(status & kStatusBsy));
		assert(!(status & kStatusErr));
		assert(!(status & kStatusDf));
		assert(status & kStatusRdy);
		assert(status & kStatusDrq);
		
		auto dest = reinterpret_cast<uint8_t *>(request->buffer)
				+ request->sectorsRead * 512;
		for(int i = 0; i < 256; i++) {
			uint16_t data = _ioSpace.load(regs::inData);
			memcpy(dest + 2 * i, &data, sizeof(uint16_t));
		}
		
		HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), 0, 0));

		request->sectorsRead++;
		assert(request->sectorsRead <= request->numSectors);
		if(request->sectorsRead == request->numSectors) {
//			std::cout << "block-ata: Reading from " << request->sector
//					<< " complete" << std::endl;
			request->promise.set_value();
			_requestQueue.pop();
			_inRequest = false;

			if(!_requestQueue.empty())
				_performRequest();
		}
	}
}))

async::result<void> Controller::readSectors(uint64_t sector, void *buffer, size_t num_sectors) {
//	std::cout << "block-ata: Reading " << num_sectors
//			<< " sectors from " << sector << std::endl;
	_requestQueue.emplace();
	auto request = &_requestQueue.back();
	request->sector = sector;
	request->numSectors = num_sectors;
	request->sectorsRead = 0;
	request->buffer = buffer;
	auto future = request->promise.async_get();

	if(!_inRequest)
		_performRequest();
	return future;
}

void Controller::_performRequest() {
	assert(!_inRequest);
	_inRequest = true;

	assert(!_requestQueue.empty());
	auto request = &_requestQueue.front();

	assert(!(request->sector & ~((size_t(1) << 48) - 1)));
	_ioSpace.store(regs::outDevice, kDeviceLba);
	
	_ioSpace.store(regs::outSectorCount, (request->numSectors >> 8) & 0xFF);
	_ioSpace.store(regs::outLba1, (request->sector >> 24) & 0xFF);
	_ioSpace.store(regs::outLba2, (request->sector >> 32) & 0xFF);
	_ioSpace.store(regs::outLba3, (request->sector >> 40) & 0xFF);	
	
	_ioSpace.store(regs::outSectorCount, request->numSectors & 0xFF);
	_ioSpace.store(regs::outLba1, request->sector & 0xFF);
	_ioSpace.store(regs::outLba2, (request->sector >> 8) & 0xFF);
	_ioSpace.store(regs::outLba3, (request->sector >> 16) & 0xFF);
	
	_ioSpace.store(regs::outCommand, kCommandReadSectorsExt);
}

Controller globalController;

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("block-ata: Starting driver\n");

	globalController.run();

	while(true)
		helix::Dispatcher::global().dispatch();
}

