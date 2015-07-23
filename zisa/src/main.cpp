
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <queue>

#include "keyboard.pb.h"

uint8_t ioInByte(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t out_value asm("al");
	asm volatile ( "inb %%dx, %%al" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}
uint16_t ioInShort(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t out_value asm("ax");
	asm volatile ( "inw %%dx, %%ax" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}

void ioOutByte(uint16_t port, uint8_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t in_value asm("al") = value;
	asm volatile ( "outb %%al, %%dx" : : "r" (in_port), "r" (in_value) );
}

class AtaDriver {
public:
	AtaDriver(helx::EventHub &event_hub);

	void readSectors(int64_t sector, uint8_t *buffer,
			size_t num_sectors, helx::Callback<> callback);

private:
	void performRequest();
	void onReadIrq(int64_t submit_id);

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
		int64_t sector;
		size_t numSectors;
		size_t sectorsRead;
		uint8_t *buffer;
		helx::Callback<> callback;
	};

	std::queue<Request> p_requestQueue;

	helx::EventHub &p_eventHub;
	HelHandle p_irqHandle;
	HelHandle p_ioHandle;
	uint16_t p_basePort;
	bool p_inRequest;
};

AtaDriver::AtaDriver(helx::EventHub &event_hub)
		: p_eventHub(event_hub), p_basePort(0x1F0), p_inRequest(false) {
	helAccessIrq(14, &p_irqHandle);

	uintptr_t ports[] = { 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6 };
	helAccessIo(ports, 9, &p_ioHandle);
	helEnableIo(p_ioHandle);
}

void AtaDriver::readSectors(int64_t sector, uint8_t *buffer,
			size_t num_sectors, helx::Callback<> callback) {
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

void AtaDriver::performRequest() {
	p_inRequest = true;

	Request &request = p_requestQueue.front();

	helx::IrqCb callback = HELX_MEMBER(this, &AtaDriver::onReadIrq);
	helSubmitWaitForIrq(p_irqHandle, p_eventHub.getHandle(), 0,
		(uintptr_t)callback.getFunction(),
		(uintptr_t)callback.getObject());

	ioOutByte(p_basePort + kPortWriteDevice, kDeviceLba);
	
	ioOutByte(p_basePort + kPortWriteSectorCount, (request.numSectors >> 8) & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba1, (request.sector >> 24) & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba2, (request.sector >> 32) & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba3, (request.sector >> 40) & 0xFF);	
	
	ioOutByte(p_basePort + kPortWriteSectorCount, request.numSectors & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba1, request.sector & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba2, (request.sector >> 8) & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba3, (request.sector >> 16) & 0xFF);
	
	ioOutByte(p_basePort + kPortWriteCommand, kCommandReadSectorsExt);
}

void AtaDriver::onReadIrq(int64_t submit_id) {
	uint8_t status = ioInByte(p_basePort + kPortReadStatus);
	/*if((status & kStatusBsy) != 0)
		continue;
	if((status & kStatusDrq) == 0)
		continue;
	break;
	}*/

	Request &request = p_requestQueue.front();
	
	size_t offset = request.sectorsRead * 512;
	for(int i = 0; i < 256; i++) {
		uint16_t word = ioInShort(p_basePort + kPortReadData);
		request.buffer[offset + 2 * i] = word & 0xFF;
		request.buffer[offset + 2 * i + 1] = (word >> 8) & 0xFF;
	}
	
	request.sectorsRead++;
	if(request.sectorsRead < request.numSectors) {
		helx::IrqCb callback = HELX_MEMBER(this, &AtaDriver::onReadIrq);
		helSubmitWaitForIrq(p_irqHandle, p_eventHub.getHandle(), 0,
			(uintptr_t)callback.getFunction(),
			(uintptr_t)callback.getObject());
	}else{
		request.callback();

		p_requestQueue.pop();

		p_inRequest = false;
		if(!p_requestQueue.empty())
			performRequest();
	}
}

class Keyboard {
public:
	Keyboard(helx::EventHub &event_hub);
	
	void run();

private:
	void onScancode(int64_t submit_id);

	helx::EventHub &p_eventHub;
	HelHandle p_irqHandle;
	HelHandle p_ioHandle;
};

Keyboard::Keyboard(helx::EventHub &event_hub)
		: p_eventHub(event_hub) {
	helAccessIrq(1, &p_irqHandle);
	
	uintptr_t ports[] = { 0x60, 0x64 };
	helAccessIo(ports, 2, &p_ioHandle);
	helEnableIo(p_ioHandle);
}

void Keyboard::run() {
	helx::IrqCb callback = HELX_MEMBER(this, &Keyboard::onScancode);
	helSubmitWaitForIrq(p_irqHandle, p_eventHub.getHandle(), 0,
		(uintptr_t)callback.getFunction(),
		(uintptr_t)callback.getObject());
}

void Keyboard::onScancode(int64_t submit_id) {
	while(true) {
		uint8_t status = ioInByte(0x64);
		if((status & 0x01) == 0)
			break;

		uint8_t code = ioInByte(0x60);
		printf("0x%X\n", code);
	}

	run();
}

helx::EventHub eventHub;

// --------------------------------------------------------
// ATA testing code
// --------------------------------------------------------

AtaDriver ataDriver(eventHub);
uint8_t ataBuffer[512];

void onAtaRead(void *object) {
	printf("Read complete!\n");
}

void testAta() {
	ataDriver.readSectors(0, ataBuffer, 1,
			helx::Callback<>(nullptr, &onAtaRead));
}

// --------------------------------------------------------
// IPC testing code
// --------------------------------------------------------

uint8_t recvBuffer[10];

void onReceive(void *object, int64_t submit_id,
		HelError error, size_t length) {
	printf("ok %d %u %s\n", error, length, recvBuffer);
}

void onAccept(void *object, int64_t submit_id, HelHandle handle) {
	printf("accept\n");
	
	helSendString(handle, (const uint8_t *)"hello", 6, 1, 1);
}
void onConnect(void *object, int64_t submit_id, HelHandle handle) {
	printf("connect\n");

	helx::RecvStringCb callback(nullptr, &onReceive);
	helSubmitRecvString(handle, eventHub.getHandle(),
			recvBuffer, 10, -1, -1, 0,
			(uintptr_t)callback.getFunction(),
			(uintptr_t)callback.getObject());
}

void testIpc() {
	helx::AcceptCb accept_cb(nullptr, &onAccept);
	helx::ConnectCb connect_cb(nullptr, &onConnect);

	HelHandle socket;

	HelHandle server, client;
	helCreateServer(&server, &client);
	helSubmitAccept(server, eventHub.getHandle(), 0,
			(uintptr_t)accept_cb.getFunction(),
			(uintptr_t)accept_cb.getObject());
	helSubmitConnect(client, eventHub.getHandle(), 0,
			(uintptr_t)connect_cb.getFunction(),
			(uintptr_t)connect_cb.getObject());
}

// --------------------------------------------------------
// main
// --------------------------------------------------------

int main() {
	testAta();

	while(true)
		eventHub.defaultProcessEvents();
}

