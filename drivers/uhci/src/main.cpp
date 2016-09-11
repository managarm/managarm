
#include <stdio.h>
#include <assert.h>
#include <functional>
#include <memory>

#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <boost/intrusive/list.hpp>

#include <bragi/mbus.hpp>
#include <hw.pb.h>

#include "usb.hpp"
#include "uhci.hpp"

helx::EventHub eventHub = helx::EventHub::create();
bragi_mbus::Connection mbusConnection(eventHub);

struct Endpoint {
	size_t maxPacketSize;	
};

struct Device {
	uint8_t address;
	Endpoint endpoints[32];
};

struct Transaction {
	Transaction(std::shared_ptr<Device> device, int endpoint, SetupPacket setup, std::function<void()> callback)
	: _device(device), _endpoint(endpoint), _completeCounter(0), _setup(setup), _callback(callback) { }

	void buildQueue(void *buffer) {
		std::allocator<TransferDescriptor> transfer_allocator;
		std::allocator<QueueHead> queue_allocator;

		_numTransfers = (_setup.wLength + 7) / 8;
		_queue = queue_allocator.allocate(1);
		_transfers = transfer_allocator.allocate(_numTransfers + 2);
	
		new (_queue) QueueHead;
		_queue->_elementPointer = QueueHead::ElementPointer::from(&_transfers[0]);

		new (&_transfers[0]) TransferDescriptor(TransferStatus(true, false, false),
				TransferToken(TransferToken::kPacketSetup, TransferToken::kData0,
						0, 0, sizeof(SetupPacket)),
				TransferBufferPointer::from(&_setup));
		_transfers[0]._linkPointer = TransferDescriptor::LinkPointer::from(&_transfers[1]);

		size_t progress = 0;
		for(size_t i = 0; i < _numTransfers; i++) {
			size_t chunk = std::min((size_t)8, _setup.wLength - progress);
			new (&_transfers[i + 1]) TransferDescriptor(TransferStatus(true, false, false),
				TransferToken(TransferToken::kPacketIn,
						i % 2 == 0 ? TransferToken::kData0 : TransferToken::kData1,
						0, 0, chunk),
				TransferBufferPointer::from((char *)buffer + progress));
			_transfers[i + 1]._linkPointer = TransferDescriptor::LinkPointer::from(&_transfers[i + 2]);
			progress += chunk;
		}

		new (&_transfers[_numTransfers + 1]) TransferDescriptor(TransferStatus(true, false, false),
				TransferToken(TransferToken::kPacketOut, TransferToken::kData0,
						0, 0, 0),
				TransferBufferPointer());
	}

	QueueHead::LinkPointer head() {
		return QueueHead::LinkPointer::from(_queue);
	}

	void linkNext(QueueHead::LinkPointer link) {
		_queue->_linkPointer = link;
	}

	void dumpStatus() {
		for(size_t i = 0; i < _numTransfers + 2; i++) {
			printf("Status[%lu]: ", i);
			_transfers[i].dumpStatus();
			printf("\n");
		}
	}

	bool progress() {
		while(_completeCounter < _numTransfers) {
			TransferDescriptor *transfer = &_transfers[_completeCounter];
			if(transfer->_controlStatus.isActive())
				return false;
			assert(!transfer->_controlStatus.isAnyError());
			_completeCounter++;
		}
		printf("Transfer complete!\n");
		_callback();
		return true;
	}

	boost::intrusive::list_member_hook<> scheduleHook;

private:
	std::shared_ptr<Device> _device;
	int _endpoint;
	size_t _completeCounter;
	SetupPacket _setup;
	std::function<void()> _callback;
	size_t _numTransfers;
	QueueHead *_queue;
	TransferDescriptor *_transfers;
};

boost::intrusive::list<
	Transaction, 
	boost::intrusive::member_hook<
		Transaction,
		boost::intrusive::list_member_hook<>,
		&Transaction::scheduleHook
	>
> scheduleList;

enum XferFlags {
	kXferToDevice = 0,
	kXferToHost = 1,
};

struct ControlTransfer {
	ControlTransfer(std::shared_ptr<Device> device, int endpoint, XferFlags flags, ControlRecipient recipient,
			ControlType type, uint8_t request, uint16_t arg0, uint16_t arg1, void *buffer, size_t length)
	: device(device), endpoint(endpoint), flags(flags), recipient(recipient), type(type),
			request(request), arg0(arg0), arg1(arg1), buffer(buffer), length(length) { }

	std::shared_ptr<Device> device;
	int endpoint;
	XferFlags flags;
	ControlRecipient recipient;
	ControlType type;
	uint8_t request;
	uint16_t arg0;
	uint16_t arg1;
	void *buffer;
	size_t length;
};

struct Controller {
	Controller(uint16_t base, helx::Irq irq)
	: _base(base), _irq(frigg::move(irq)) { }

	void initialize() {
		auto initial_status = frigg::readIo<uint16_t>(_base + kRegStatus);
		assert(!(initial_status & kStatusInterrupt));
		assert(!(initial_status & kStatusError));

		enum {
			kRootConnected = 0x0001,
			kRootConnectChange = 0x0002,
			kRootEnabled = 0x0004,
			kRootEnableChange = 0x0008,
			kRootReset = 0x0200
		};
		
		// global reset, then deassert reset and stop running the frame list
		frigg::writeIo<uint16_t>(_base + kRegCommand, 0x04);
		frigg::writeIo<uint16_t>(_base + kRegCommand, 0);

		// enable interrupts
		frigg::writeIo<uint16_t>(_base + kRegInterruptEnable, 0x0F);

		// disable both ports and clear their connected/enabled changed bits
		frigg::writeIo<uint16_t>(_base + kRegPort1StatusControl,
				kRootConnectChange | kRootEnableChange);
		frigg::writeIo<uint16_t>(_base + kRegPort2StatusControl,
				kRootConnectChange | kRootEnableChange);

		// enable the first port and wait until it is available
		frigg::writeIo<uint16_t>(_base + kRegPort1StatusControl, kRootEnabled);
		while(true) {
			auto port_status = frigg::readIo<uint16_t>(_base + kRegPort1StatusControl);
			if((port_status & kRootEnabled))
				break;
		}

		// reset the first port
		frigg::writeIo<uint16_t>(_base + kRegPort1StatusControl, kRootEnabled | kRootReset);
		frigg::writeIo<uint16_t>(_base + kRegPort1StatusControl, kRootEnabled);
		
		auto postenable_status = frigg::readIo<uint16_t>(_base + kRegStatus);
		assert(!(postenable_status & kStatusInterrupt));
		assert(!(postenable_status & kStatusError));

		auto device = std::make_shared<Device>();
		device->address = 0;
		int endpoint = 0;

		ControlTransfer control1(device, endpoint, kXferToHost, kDestDevice, kStandard,
				SetupPacket::kGetDescriptor, SetupPacket::kDescDevice,
				0, _buffer, 18);
		
		transfer(device, endpoint, control1, [this, device, endpoint](){
			printf("callback control1\n");
			ControlTransfer control2(device, endpoint, kXferToHost, kDestDevice, kStandard,
					SetupPacket::kGetDescriptor, SetupPacket::kDescDevice,
					0, _buffer2, 18);
			transfer(device, endpoint, control2, [](){
				printf("callback control2\n");		
			});
		});
;
/*
		//create get config request
		alignas(64) uint8_t config_buffer[34];
		SetupPacket setup(SetupPacket::kDirToHost, SetupPacket::kDestDevice,
				SetupPacket::kStandard, SetupPacket::kGetDescriptor,
				SetupPacket::kDescConfig, 0, 34);
		
		Transaction action(setup);
		action.buildQueue(config_buffer);

		QueueHead head;
		head._elementPointer = QueueHead::ElementPointer::from(action.head());
*/

		// setup the frame list
		HelHandle list_handle;
		HEL_CHECK(helAllocateMemory(4096, 0, &list_handle));
		void *list_mapping;
		HEL_CHECK(helMapMemory(list_handle, kHelNullHandle,
				nullptr, 0, 4096, kHelMapReadWrite, &list_mapping));
		
		auto list_pointer = (FrameList *)list_mapping;
		
		for(int i = 0; i < 1024; i++) {
			list_pointer->entries[i] = FrameListPointer::from(&_initialQh);
		}
			
		// pass the frame list to the controller and run it
		uintptr_t list_physical;
		HEL_CHECK(helPointerPhysical(list_pointer, &list_physical));
		assert((list_physical % 0x1000) == 0);
		frigg::writeIo<uint32_t>(_base + kRegFrameListBaseAddr, list_physical);
		
		auto prerun_status = frigg::readIo<uint16_t>(_base + kRegStatus);
		assert(!(prerun_status & kStatusInterrupt));
		assert(!(prerun_status & kStatusError));
		
		uint16_t command_bits = 0x1;
		frigg::writeIo<uint16_t>(_base + kRegCommand, command_bits);

/*
		while(true) {
			printf("----------------------------------------\n");
			auto status = frigg::readIo<uint16_t>(_base + kRegStatus);
			printf("usb status register: %d \n", status);
			auto port_status = frigg::readIo<uint16_t>(_base + 0x10);
			printf("port status/control register:\n");
			printf("    current connect status: %d\n", port_status & (1 << 0));
			printf("    connect status change: %d\n", port_status & (1 << 1));
			printf("    port enabled: %d\n", port_status & (1 << 2));
			printf("    port enable change: %d\n", port_status & (1 << 3));
			printf("    line status: %d\n", port_status & 0x30);
			printf("    resume detect: %d\n", port_status & (1 << 6));
			printf("    always 1: %d\n", port_status & (1 << 7));
			printf("    low speed device: %d\n", port_status & (1 << 8));
			printf("    port reset: %d\n", port_status & (1 << 9));
			printf("    suspend: %d\n", port_status & (1 << 12));
			transaction->dumpStatus();
			auto dev_desc = (DeviceDescriptor *) _buffer;
			printf("   length: %d\n", dev_desc->_length); 
			printf("   descriptor type: %d\n", dev_desc->_descriptorType); 
			printf("   bcdUsb: %d\n", dev_desc->_bcdUsb); 
			printf("   device class: %d\n", dev_desc->_deviceClass); 
			printf("   device subclass: %d\n", dev_desc->_deviceSubclass); 
			printf("   device protocol: %d\n", dev_desc->_deviceProtocol); 
			printf("   max packet size: %d\n", dev_desc->_maxPacketSize); 
			printf("   vendor: %d\n", dev_desc->_idVendor); 
			printf("   num configs: %d\n", dev_desc->_numConfigs); 
	
		}
*/
		_irq.wait(eventHub, CALLBACK_MEMBER(this, &Controller::onIrq));
	}

	void transfer(std::shared_ptr<Device> device, int endpoint, ControlTransfer control, std::function<void()> callback) {
		Transaction *transaction = new Transaction(device, endpoint, SetupPacket(control.flags & kXferToDevice ? kDirToDevice : kDirToHost,
				control.recipient, control.type, control.request, control.arg0, control.arg1, control.length),
				callback);
		transaction->buildQueue(control.buffer);

		if(scheduleList.empty()) {
			_initialQh._linkPointer = transaction->head();
		}else{
			scheduleList.back().linkNext(transaction->head());
		}
		scheduleList.push_back(*transaction);
	}

	void onIrq(HelError error) {
		HEL_CHECK(error);

		auto status = frigg::readIo<uint16_t>(_base + kRegStatus);
		assert(!(status & kStatusError));

		if(status & kStatusInterrupt) {
			frigg::writeIo<uint16_t>(_base + kRegStatus, kStatusInterrupt);
			printf("zomg usb irq!111\n");
		
		Transaction *incomplete = nullptr;

		auto it = scheduleList.begin();
			while(it != scheduleList.end()) {
				auto copy = it;
				++it;
				if(copy->progress()) {
					scheduleList.erase(copy);

					QueueHead::LinkPointer link;
					if(it != scheduleList.end())
						link = it->head();
					if(incomplete) {
						incomplete->linkNext(link);
					}else{
						_initialQh._linkPointer = link;
					}
				}else {
					incomplete = &(*copy);
				}
			}
		}

		_irq.wait(eventHub, CALLBACK_MEMBER(this, &Controller::onIrq));
	}

private:
	uint16_t _base;
	helx::Irq _irq;

	QueueHead _initialQh;
	alignas(32) uint8_t _buffer[18];
	alignas(32) uint8_t _buffer2[18];
};

// --------------------------------------------------------
// InitClosure
// --------------------------------------------------------

struct InitClosure {
	void operator() ();

private:
	void connected();
	void enumeratedDevice(std::vector<bragi_mbus::ObjectId> objects);
	void queriredDevice(HelHandle handle);
};

void InitClosure::operator() () {
	mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void InitClosure::connected() {
	mbusConnection.enumerate({ "pci-vendor:0x8086", "pci-device:0x7020" },
			CALLBACK_MEMBER(this, &InitClosure::enumeratedDevice));
}

void InitClosure::enumeratedDevice(std::vector<bragi_mbus::ObjectId> objects) {
	assert(objects.size() == 1);
	mbusConnection.queryIf(objects[0],
			CALLBACK_MEMBER(this, &InitClosure::queriredDevice));
}

void InitClosure::queriredDevice(HelHandle handle) {
	helx::Pipe device_pipe(handle);

	// acquire the device's resources
	printf("acquire the device's resources\n");
	HelError acquire_error;
	uint8_t acquire_buffer[128];
	size_t acquire_length;
	device_pipe.recvStringRespSync(acquire_buffer, 128, eventHub, 1, 0,
			acquire_error, acquire_length);
	HEL_CHECK(acquire_error);

	managarm::hw::PciDevice acquire_response;
	acquire_response.ParseFromArray(acquire_buffer, acquire_length);

	HelError bar_error;
	HelHandle bar_handle;
	device_pipe.recvDescriptorRespSync(eventHub, 1, 5, bar_error, bar_handle);
	HEL_CHECK(bar_error);

	assert(acquire_response.bars(4).io_type() == managarm::hw::IoType::PORT);
	HEL_CHECK(helEnableIo(bar_handle));

	HelError irq_error;
	HelHandle irq_handle;
	device_pipe.recvDescriptorRespSync(eventHub, 1, 7, irq_error, irq_handle);
	HEL_CHECK(irq_error);
	
	Controller *controller = new Controller(acquire_response.bars(4).address(),
			helx::Irq(irq_handle));
	controller->initialize();
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting uhci (usb-)driver\n");

	auto closure = new InitClosure();
	(*closure)();

	while(true)
		eventHub.defaultProcessEvents();
	
	return 0;
}

