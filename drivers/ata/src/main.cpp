
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>
#include <queue>
#include <unordered_map>
#include <experimental/optional>

#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/array.hpp>
#include <frigg/callback.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <helx.hpp>

#include <libfs.hpp>

helx::EventHub eventHub = helx::EventHub::create();

// --------------------------------------------------------
// Driver class
// --------------------------------------------------------

class Driver : public libfs::BlockDevice {
public:
	Driver();

	void readSectors(uint64_t sector, void *buffer,
			size_t num_sectors, frigg::CallbackPtr<void()> callback) override;

private:
	enum Ports {
		kPortReadData = 0,
		kPortWriteSectorCount = 2,
		kPortWriteLba1 = 3,
		kPortWriteLba2 = 4,
		kPortWriteLba3 = 5,
		kPortWriteDevice = 6,
		kPortWriteCommand = 7,
		kPortReadStatus = 7,
	};

	enum Commands {
		kCommandReadSectorsExt = 0x24
	};

	enum Flags {
		kStatusDrq = 0x08,
		kStatusBsy = 0x80,

		kDeviceSlave = 0x10,
		kDeviceLba = 0x40
	};

	struct Request {
		uint64_t sector;
		size_t numSectors;
		size_t sectorsRead;
		void *buffer;
		frigg::CallbackPtr<void()> callback;
	};
	
	void performRequest();
	void onReadIrq(HelError error);

	std::queue<Request> p_requestQueue;

	HelHandle p_irqHandle;
	HelHandle p_ioHandle;
	uint16_t p_basePort;
	bool p_inRequest;
};

Driver::Driver()
: BlockDevice(512), p_basePort(0x1F0), p_inRequest(false) {
	HEL_CHECK(helAccessIrq(14, &p_irqHandle));

	uintptr_t ports[] = { 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6 };
	HEL_CHECK(helAccessIo(ports, 9, &p_ioHandle));
	HEL_CHECK(helEnableIo(p_ioHandle));
}

void Driver::readSectors(uint64_t sector, void *buffer,
			size_t num_sectors, frigg::CallbackPtr<void()> callback) {
	Request request;
	request.sector = sector;
	request.numSectors = num_sectors;
	request.sectorsRead = 0;
	request.buffer = buffer;
	request.callback = callback;
	p_requestQueue.push(request);

	if(!p_inRequest)
		performRequest();
}

void Driver::performRequest() {
	p_inRequest = true;

	Request &request = p_requestQueue.front();

	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteDevice, kDeviceLba);
	
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteSectorCount, (request.numSectors >> 8) & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba1, (request.sector >> 24) & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba2, (request.sector >> 32) & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba3, (request.sector >> 40) & 0xFF);	
	
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteSectorCount, request.numSectors & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba1, request.sector & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba2, (request.sector >> 8) & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba3, (request.sector >> 16) & 0xFF);
	
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteCommand, kCommandReadSectorsExt);

	auto callback = CALLBACK_MEMBER(this, &Driver::onReadIrq);
	int64_t async_id;
	HEL_CHECK(helSubmitWaitForIrq(p_irqHandle, eventHub.getHandle(),
			(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
			&async_id));
}

void Driver::onReadIrq(HelError error) {
	Request &request = p_requestQueue.front();
	
	// acknowledge the interrupt
	/*uint8_t status =*/ frigg::arch_x86::ioInByte(p_basePort + kPortReadStatus);
//	assert((status & kStatusBsy) == 0);
//	assert((status & kStatusDrq) != 0);
	
	size_t offset = request.sectorsRead * 512;
	auto dest = (uint8_t *)request.buffer + offset;
	frigg::arch_x86::ioPeekMultiple(p_basePort + kPortReadData, (uint16_t *)dest, 256);
	
	request.sectorsRead++;
	if(request.sectorsRead < request.numSectors) {
		auto callback = CALLBACK_MEMBER(this, &Driver::onReadIrq);
		int64_t async_id;
		HEL_CHECK(helSubmitWaitForIrq(p_irqHandle, eventHub.getHandle(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
				&async_id));
	}else{
		assert(request.sectorsRead == request.numSectors);
		request.callback();
		p_requestQueue.pop();

		p_inRequest = false;
		if(!p_requestQueue.empty())
			performRequest();
	}
}

Driver driver;

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting ATA driver\n");

	libfs::runDevice(eventHub, &driver);

	while(true)
		eventHub.defaultProcessEvents();
}

