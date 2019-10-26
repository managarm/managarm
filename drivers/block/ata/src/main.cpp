
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <iostream>
#include <queue>

#include <async/result.hpp>
#include <async/doorbell.hpp>
#include <arch/io_space.hpp>
#include <arch/register.hpp>
#include <helix/ipc.hpp>

#include <blockfs.hpp>

namespace {
	constexpr bool logIrqs = false;
	constexpr bool logRequests = false;
}

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

namespace alt_regs {
	inline constexpr arch::scalar_register<uint8_t> inStatus{0};
}

class Controller : public blockfs::BlockDevice {
public:
	Controller();

public:
	async::detached run();

private:
	async::detached _doRequestLoop();

public:
	async::result<void> readSectors(uint64_t sector, void *buffer,
			size_t num_sectors) override;

private:
	enum Commands {
		kCommandReadSectors = 0x20,
		kCommandReadSectorsExt = 0x24,
		kCommandIdentify = 0xEC,
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
		void *buffer;
		async::promise<void> promise;
	};

	async::result<void> _performRequest(Request *request);

	async::result<bool> _detectDevice();

	std::queue<std::unique_ptr<Request>> _requestQueue;
	async::doorbell _doorbell;

	helix::UniqueDescriptor _irq;
	HelHandle _ioHandle;
	arch::io_space _ioSpace;
	arch::io_space _altSpace;

	bool _supportsLBA48;

	uint64_t _irqSequence = 0;
};

Controller::Controller()
: BlockDevice{512}, _ioSpace{0x1F0}, _altSpace{0x3F6}, _supportsLBA48{false} {
	HelHandle irq_handle;
	HEL_CHECK(helAccessIrq(14, &irq_handle));
	_irq = helix::UniqueDescriptor{irq_handle};

	uintptr_t ports[] = { 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6 };
	HEL_CHECK(helAccessIo(ports, 9, &_ioHandle));
	HEL_CHECK(helEnableIo(_ioHandle));
}

async::detached Controller::run() {
	if (!(co_await _detectDevice()))
		co_return;

	_doRequestLoop();

	blockfs::runDevice(this);
}

async::detached Controller::_doRequestLoop() {
	while(true) {
		if(_requestQueue.empty()) {
			co_await _doorbell.async_wait();
			continue;
		}

		auto request = _requestQueue.front().get();
		co_await _performRequest(request);
		request->promise.set_value();
		_requestQueue.pop();
	}
}

async::result<void> Controller::readSectors(uint64_t sector, void *buffer, size_t num_sectors) {
	auto request = std::make_unique<Request>();
	auto future = request->promise.async_get();
	request->sector = sector;
	request->numSectors = num_sectors;
	request->buffer = buffer;

	_requestQueue.push(std::move(request));
	_doorbell.ring();

	return future;
}

async::result<bool> Controller::_detectDevice() {
	_ioSpace.store(regs::outDevice, kDeviceLba);
	// TODO: delay

	_ioSpace.store(regs::outCommand, kCommandIdentify);

	if(logIrqs)
		std::cout << "block/ata: Awaiting IRQ." << std::endl;
	helix::AwaitEvent await_irq;
	auto &&submit = helix::submitAwaitEvent(_irq, &await_irq, _irqSequence,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(await_irq.error());
	_irqSequence = await_irq.sequence();
	if(logIrqs)
		std::cout << "block/ata: IRQ fired." << std::endl;

	// Check if the device is ready without clearing the IRQ.
	auto alt_status = _altSpace.load(alt_regs::inStatus);
	if(alt_status & kStatusBsy) {
		HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckNack, _irqSequence));
		co_return false;
	}

	// Clear and acknowledge the IRQ.
	auto status = _ioSpace.load(regs::inStatus);
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, _irqSequence));

	if (status & kStatusBsy || !(status & kStatusDrq))
		co_return false;

	uint8_t ident_data[512];
	_ioSpace.load_iterative(regs::inData, reinterpret_cast<uint16_t *>(ident_data), 256);

	char model[41];
	memcpy(model, ident_data + 54, 40);
	model[40] = 0;

	// model name is returned as big endian, swap each 2-byte pair to fix that
	for (int i = 0; i < 40; i += 2) {
		uint8_t tmp = model[i];
		model[i] = model[i + 1];
		model[i + 1] = tmp;
	}

	_supportsLBA48 = (ident_data[167] & (1 << 2))
			&& (ident_data[173] & (1 << 2));

	printf("block/ata: detected device, model: '%s', %s 48-bit LBA\n", model, _supportsLBA48 ? "supports" : "doesn't support");

	co_return true;
}

async::result<void> Controller::_performRequest(Request *request) {
	if(logRequests)
		std::cout << "block/ata: Reading " << request->numSectors
				<< " sectors from " << request->sector << std::endl;

	assert(!(request->sector & ~((size_t(1) << 48) - 1)));
	_ioSpace.store(regs::outDevice, kDeviceLba);
	// TODO: There should be a delay after drive selection.

	if (_supportsLBA48) {
		_ioSpace.store(regs::outSectorCount, (request->numSectors >> 8) & 0xFF);
		_ioSpace.store(regs::outLba1, (request->sector >> 24) & 0xFF);
		_ioSpace.store(regs::outLba2, (request->sector >> 32) & 0xFF);
		_ioSpace.store(regs::outLba3, (request->sector >> 40) & 0xFF);
	}

	_ioSpace.store(regs::outSectorCount, request->numSectors & 0xFF);
	_ioSpace.store(regs::outLba1, request->sector & 0xFF);
	_ioSpace.store(regs::outLba2, (request->sector >> 8) & 0xFF);
	_ioSpace.store(regs::outLba3, (request->sector >> 16) & 0xFF);

	if (_supportsLBA48)
		_ioSpace.store(regs::outCommand, kCommandReadSectorsExt);
	else
		_ioSpace.store(regs::outCommand, kCommandReadSectors);

	// Receive the result for each sector.
	for(size_t k = 0; k < request->numSectors; k++) {
		while(true) {
			if(logIrqs)
				std::cout << "block/ata: Awaiting IRQ." << std::endl;
			helix::AwaitEvent await_irq;
			auto &&submit = helix::submitAwaitEvent(_irq, &await_irq, _irqSequence,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(await_irq.error());
			_irqSequence = await_irq.sequence();
			if(logIrqs)
				std::cout << "block/ata: IRQ fired." << std::endl;

			// Check if the device is ready without clearing the IRQ.
			auto alt_status = _altSpace.load(alt_regs::inStatus);
			if(alt_status & kStatusBsy) {
				HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckNack, _irqSequence));
				continue;
			}
			assert(!(alt_status & kStatusErr));
			assert(!(alt_status & kStatusDf));

			// Clear and acknowledge the IRQ.
			auto status = _ioSpace.load(regs::inStatus);
			HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckAcknowledge, _irqSequence));
			assert(!(status & kStatusErr));
			assert(!(status & kStatusDf));
			if(!(status & kStatusRdy))
				std::cout << "\e[31m" "block/ata: RDY is not set after IRQ" "\e[39m" << std::endl;
			if(!(status & kStatusDrq))
				std::cout << "\e[31m" "block/ata: DRQ is not set after IRQ" "\e[39m" << std::endl;
			if((status & kStatusRdy) && (status & kStatusDrq))
				break;
		}

		// Read the data.
		// TODO: Do we have to be careful with endianess here?
		auto dest = reinterpret_cast<uint8_t *>(request->buffer) + k * 512;
		// TODO: The following is a hack. Lock the page into memory instead!
		*static_cast<volatile uint8_t *>(dest) = 0; // Fault in the page.
		_ioSpace.load_iterative(regs::inData, reinterpret_cast<uint16_t *>(dest), 256);
	}

	if(logRequests)
		std::cout << "block/ata: Reading from " << request->sector
				<< " complete" << std::endl;
}

Controller globalController;

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("block/ata: Starting driver\n");

	{
		async::queue_scope scope{helix::globalQueue()};
		globalController.run();
	}

	helix::globalQueue()->run();
}

