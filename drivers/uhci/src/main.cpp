#include <assert.h>
#include <stdio.h>
#include <deque>
#include <experimental/optional>
#include <functional>
#include <iostream>
#include <memory>

#include <cofiber.hpp>
#include <boost/intrusive/list.hpp>
#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/memory.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <mbus.hpp>

#include "usb.hpp"
#include "uhci.hpp"
#include "hid.hpp"
#include "schedule.hpp"
#include <hw.pb.h>

struct Field {
	int bitOffset;
	int bitSize;
	uint16_t usagePage;
	uint16_t usageId;
};

std::vector<uint32_t> parse(std::vector<Field> fields, uint8_t *report) {
	std::vector<uint32_t> values;
	for(Field &f : fields) {
		int b = f.bitOffset / 8;
		uint32_t raw = uint32_t(report[b]) | (uint32_t(report[b + 1]) << 8)
				| (uint32_t(report[b + 2]) << 16) | (uint32_t(report[b + 3]) << 24);
		uint32_t mask = (uint32_t(1) << f.bitSize) - 1;
		values.push_back((raw >> (f.bitOffset % 8)) & mask);
	}
	return values;
}

struct ContiguousPolicy {
public:
	uintptr_t map(size_t length) {
		assert((length % 0x1000) == 0);

		HelHandle memory;
		void *actual_ptr;
		HEL_CHECK(helAllocateMemory(length, kHelAllocContinuous, &memory));
		HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, 0, length,
				kHelMapReadWrite | kHelMapCopyOnWriteAtFork, &actual_ptr));
		HEL_CHECK(helCloseDescriptor(memory));
		return (uintptr_t)actual_ptr;
	}

	void unmap(uintptr_t address, size_t length) {
		HEL_CHECK(helUnmapMemory(kHelNullHandle, (void *)address, length));
	}
};

using ContiguousAllocator = frigg::SlabAllocator<
	ContiguousPolicy,
	frigg::TicketLock
>;

ContiguousPolicy contiguousPolicy;
ContiguousAllocator contiguousAllocator(contiguousPolicy);

// ----------------------------------------------------------------------------
// QueuedTransaction.
// ----------------------------------------------------------------------------

QueuedTransaction::QueuedTransaction(std::function<void()> callback)
	: _callback(callback), _completeCounter(0) { }

void QueuedTransaction::setupTransfers(TransferDescriptor *transfers, size_t num_transfers) {
	_transfers = transfers;
	_numTransfers = num_transfers;
}

QueueHead::LinkPointer QueuedTransaction::head() {
	return QueueHead::LinkPointer::from(&_transfers[0]);
}

void QueuedTransaction::dumpTransfer() {
	printf("    Setup stage:");
	_transfers[0].dumpStatus();
	printf("\n");
	
	for(size_t i = 0; i < _numTransfers; i++) {
		 printf("    Data stage [%lu]:", i);
		_transfers[i + 1].dumpStatus();
		printf("\n");
	}

	printf("    Status stage:");
	_transfers[_numTransfers + 1].dumpStatus();
	printf("\n");
}

bool QueuedTransaction::progress() {
//	dumpTransfer();
	while(_completeCounter < _numTransfers) {
		TransferDescriptor *transfer = &_transfers[_completeCounter];
		if(transfer->_controlStatus.isActive())
			return false;

		if(transfer->_controlStatus.isAnyError()) {
			printf("Transfer error!\n");
			return true;
		}
		
		_completeCounter++;
	}

	printf("Transfer complete!\n");
	_callback();
	return true;
}

// ----------------------------------------------------------------------------
// ControlTransaction.
// ----------------------------------------------------------------------------

ControlTransaction::ControlTransaction(std::function<void()> callback, SetupPacket setup)
	: QueuedTransaction(callback), _setup(setup) { }

void ControlTransaction::buildQueue(void *buffer, int address, int endpoint, size_t packet_size, XferFlags flags) {
	assert((flags & kXferToDevice) || (flags & kXferToHost));

	size_t data_packets = (_setup.wLength + packet_size - 1) / packet_size;
	size_t desc_size = (data_packets + 2) * sizeof(TransferDescriptor);
	auto transfers = (TransferDescriptor *)contiguousAllocator.allocate(desc_size);

	new (&transfers[0]) TransferDescriptor(TransferStatus(true, false, false),
			TransferToken(TransferToken::kPacketSetup, TransferToken::kData0,
					address, endpoint, sizeof(SetupPacket)),
			TransferBufferPointer::from(&_setup));
	transfers[0]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[1]);

	size_t progress = 0;
	for(size_t i = 0; i < data_packets; i++) {
		size_t chunk = std::min(packet_size, _setup.wLength - progress);
		new (&transfers[i + 1]) TransferDescriptor(TransferStatus(true, false, false),
			TransferToken(flags & kXferToDevice ? TransferToken::kPacketOut : TransferToken::kPacketIn,
					i % 2 == 0 ? TransferToken::kData0 : TransferToken::kData1,
					address, endpoint, chunk),
			TransferBufferPointer::from((char *)buffer + progress));
		transfers[i + 1]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[i + 2]);
		progress += chunk;
	}

	new (&transfers[data_packets + 1]) TransferDescriptor(TransferStatus(true, false, false),
			TransferToken(flags & kXferToDevice ? TransferToken::kPacketIn : TransferToken::kPacketOut,
					TransferToken::kData0, address, endpoint, 0),
			TransferBufferPointer());

	setupTransfers(transfers, data_packets + 2);
}

// ----------------------------------------------------------------------------
// QueueEntity.
// ----------------------------------------------------------------------------

QueueEntity::QueueEntity() {
	_queue = (QueueHead *)contiguousAllocator.allocate(sizeof(QueueHead));
	
	new (_queue) QueueHead;
	_queue->_linkPointer = QueueHead::LinkPointer();
	_queue->_elementPointer = QueueHead::ElementPointer();
}

QueueHead::LinkPointer QueueEntity::head() {
	return QueueHead::LinkPointer::from(_queue);
}

void QueueEntity::linkNext(QueueHead::LinkPointer link) {
	_queue->_linkPointer = link;
}

void QueueEntity::progress() {
	if(transactionList.empty())
		return;

	if(!transactionList.front().progress())
		return;
	
	transactionList.pop_front();
	assert(_queue->_elementPointer.isTerminate());

	if(!transactionList.empty()) {
		_queue->_elementPointer = transactionList.front().head();
	}
}

boost::intrusive::list<
	ScheduleEntity,
	boost::intrusive::member_hook<
		ScheduleEntity,
		boost::intrusive::list_member_hook<>,
		&ScheduleEntity::scheduleHook
	>
> scheduleList;

struct Slot {
	size_t maxPacketSize;
	std::unique_ptr<QueueEntity> queue;
};

struct Device {
	uint8_t address;
	std::unique_ptr<Slot> slots[32];
};

// arg0 = wValue in the USB spec
// arg1 = wIndex in the USB spec
struct ControlTransfer {
	ControlTransfer(std::shared_ptr<Device> device, int endpoint, XferFlags flags,
			ControlRecipient recipient, ControlType type, uint8_t request,
			uint16_t arg0, uint16_t arg1, void *buffer, size_t length)
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
	Controller(uint16_t base, helix::UniqueIrq irq)
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

		handleIrqs();
	}

	void activateEntity(ScheduleEntity *entity) {
		if(scheduleList.empty()) {
			_initialQh._linkPointer = entity->head();
		}else{
			scheduleList.back().linkNext(entity->head());
		}
		scheduleList.push_back(*entity);
	}

	void transfer(ControlTransfer control, std::function<void()> callback) {
		assert((control.flags & kXferToDevice) || (control.flags & kXferToHost));
		auto endpoint = control.device->slots[control.endpoint].get();

		SetupPacket setup(control.flags & kXferToDevice ? kDirToDevice : kDirToHost,
				control.recipient, control.type, control.request,
				control.arg0, control.arg1, control.length);
		ControlTransaction *transaction = new ControlTransaction(callback, setup);
		transaction->buildQueue(control.buffer, control.device->address,
				control.endpoint, endpoint->maxPacketSize, control.flags);

		if(endpoint->queue->transactionList.empty()) {
			endpoint->queue->_queue->_elementPointer = transaction->head();
		}

		endpoint->queue->transactionList.push_back(*transaction);
	}

	COFIBER_ROUTINE(cofiber::no_future, handleIrqs(), ([=] {
		while(true) {
			helix::AwaitIrq<helix::AwaitMechanism> edge(helix::Dispatcher::global(), _irq);
			COFIBER_AWAIT edge.future();
			HEL_CHECK(edge.error());

			auto status = frigg::readIo<uint16_t>(_base + kRegStatus);
			assert(!(status & 0x10));
			assert(!(status & 0x08));
			if(!(status & (kStatusInterrupt | kStatusError)))
				continue;

			if(status & kStatusError)
				printf("uhci: Error interrupt\n");
			frigg::writeIo<uint16_t>(_base + kRegStatus, kStatusInterrupt | kStatusError);
			
			printf("uhci: Processing transfers.\n");
			auto it = scheduleList.begin();
			while(it != scheduleList.end()) {
				it->progress();
			}
		}
	}))

private:
	uint16_t _base;
	helix::UniqueIrq _irq;

	QueueHead _initialQh;
	alignas(32) uint8_t _buffer[18];
	alignas(32) uint8_t _buffer2[18];
};

struct WaitForXfer {
	friend auto cofiber_awaiter(WaitForXfer action) {
		struct Awaiter {
			Awaiter(std::shared_ptr<Controller> controller, ControlTransfer xfer)
			: _controller(std::move(controller)), _xfer(std::move(xfer)) { }

			bool await_ready() { return false; }
			void await_resume() { }

			void await_suspend(cofiber::coroutine_handle<> handle) {
				_controller->transfer(std::move(_xfer), [handle] () {
					handle.resume();
				});
			}

		private:
			std::shared_ptr<Controller> _controller;
			ControlTransfer _xfer;
		};
	
		return Awaiter(std::move(action._controller), std::move(action._xfer));
	}

	WaitForXfer(std::shared_ptr<Controller> controller, ControlTransfer xfer)
	: _controller(std::move(controller)), _xfer(std::move(xfer)) { }

private:
	std::shared_ptr<Controller> _controller;
	ControlTransfer _xfer;
};

uint32_t fetch(uint8_t *&p, void *limit, int n = 1) {
	uint32_t x = 0;
	for(int i = 0; i < n; i++) {
		x = (x << 8) | *p++;
		assert(p <= limit);
	}
	return x;
}

COFIBER_ROUTINE(cofiber::no_future, parseReportDescriptor(std::shared_ptr<Controller> controller,
		std::shared_ptr<Device> device), [=] () {
	size_t length = 52;
	auto buffer = (uint8_t *)contiguousAllocator.allocate(length);
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToHost,
			kDestInterface, kStandard, SetupPacket::kGetDescriptor, 34 << 8, 0,
			buffer, length));

	std::vector<Field> fields;
	int bit_offset = 0;

	std::experimental::optional<int> report_count;
	std::experimental::optional<int> report_size;
	std::experimental::optional<uint16_t> usage_page;
	std::deque<uint32_t> usage;
	std::experimental::optional<uint32_t> usage_min;
	std::experimental::optional<uint32_t> usage_max;

	uint8_t *p = buffer;
	uint8_t *limit = buffer + length;
	while(p < limit) {
		uint8_t token = fetch(p, limit);
		int size = (token & 0x03) == 3 ? 4 : (token & 0x03);
		uint32_t data = fetch(p, limit, size);
		switch(token & 0xFC) {
		// Main items
		case 0xC0:
			printf("End Collection: 0x%x\n", data);
			break;

		case 0xA0:
			printf("Collection: 0x%x\n", data);
			usage.clear();
			usage_min = std::experimental::nullopt;
			usage_max = std::experimental::nullopt;
			break;

		case 0x80:
			printf("Input: 0x%x\n", data);
			if(!report_size || !report_count)
				throw std::runtime_error("Missing Report Size/Count");
				
			if(!usage_min != !usage_max)
				throw std::runtime_error("Usage Minimum without Usage Maximum or visa versa");
			
			if(!usage.empty() && (usage_min || usage_max))
				throw std::runtime_error("Usage and Usage Mnimum/Maximum specified");

			if(usage.empty() && !usage_min && !usage_max) {
				// this field is just padding
				bit_offset += (*report_size) * (*report_count);
			}else{
				for(auto i = 0; i < *report_count; i++) {
					uint16_t actual_id;
					if(!usage.empty()) {
						actual_id = usage.front();
						usage.pop_front();
					}else{
						actual_id = *usage_min + i;
					}

					Field field;
					field.bitOffset = bit_offset;
					field.bitSize = *report_size;
					field.usagePage = *usage_page;
					field.usageId = actual_id;
					fields.push_back(field);
					
					bit_offset += *report_size;
				}

				usage.clear();
				usage_min = std::experimental::nullopt;
				usage_max = std::experimental::nullopt;
			}
			break;

		// Global items
		case 0x94:
			printf("Report Count: 0x%x\n", data);
			report_count = data;
			break;
		
		case 0x74:
			printf("Report Size: 0x%x\n", data);
			report_size = data;
			break;
		
		case 0x24:
			printf("Logical Maximum: 0x%x\n", data);
			break;
		
		case 0x14:
			printf("Logical Minimum: 0x%x\n", data);
			break;
		
		case 0x04:
			printf("Usage Page: 0x%x\n", data);
			usage_page = data;
			break;

		// Local items
		case 0x28:
			printf("Usage Maximum: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			usage_max = data;
			break;
		
		case 0x18:
			printf("Usage Minimum: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			usage_min = data;
			break;
			
		case 0x08:
			printf("Usage: 0x%x\n", data);
			assert(size < 4); // TODO: this would override the usage page
			usage.push_back(data);
			break;

		default:
			printf("Unexpected token: 0x%x\n", token & 0xFC);
			abort();
		}
	}
	
	size_t rep_length = (bit_offset + 7) / 8;
	auto rep_buffer = (uint8_t *)contiguousAllocator.allocate(rep_length);
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToHost,
			kDestInterface, kClass, SetupPacket::kGetReport, 0x01 << 8, 0,
			rep_buffer, rep_length));

	auto values = parse(fields, rep_buffer);
	int counter = 0;
	for(uint32_t val : values) {
		printf("value %d: %x\n", counter, val);
		counter++;
	}

	for(auto f : fields) {
		printf("usagePage: %x\n", f.usagePage);
		printf("    usageId: %x\n", f.usageId);
	}
})

COFIBER_ROUTINE(cofiber::no_future, runHidDevice(std::shared_ptr<Controller> controller), [=] () {
	auto device = std::make_shared<Device>();
	device->address = 0;
	device->slots[0] = std::make_unique<Slot>();
	device->slots[0]->maxPacketSize = 8;
	device->slots[0]->queue = std::make_unique<QueueEntity>();
	controller->activateEntity(device->slots[0]->queue.get());

	// set the device address.
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToDevice,
			kDestDevice, kStandard, SetupPacket::kSetAddress, 1, 0,
			nullptr, 0));
	device->address = 1;

	// enquire the maximum packet size of endpoint 0 and get the device descriptor.
	auto descriptor = (DeviceDescriptor *)contiguousAllocator.allocate(sizeof(DeviceDescriptor));
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorDevice << 8, 0,
			descriptor, 8));
	device->slots[0]->maxPacketSize = descriptor->maxPacketSize;
	
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorDevice << 8, 0,
			descriptor, sizeof(DeviceDescriptor)));
	assert(descriptor->length == sizeof(DeviceDescriptor));

	// get and parse the configuration descriptor.
	auto config = (ConfigDescriptor *)contiguousAllocator.allocate(sizeof(ConfigDescriptor));
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorConfig << 8, 0,
			config, sizeof(ConfigDescriptor)));
	assert(config->length == sizeof(ConfigDescriptor));

	printf("Configuration value: %d\n", config->configValue);

	auto buffer = (uint8_t *)contiguousAllocator.allocate(config->totalLength);
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorConfig << 8, 0,
			buffer, config->totalLength));

	auto p = buffer + config->length;
	auto limit = buffer + config->totalLength;
	while(p < limit) {
		auto base = (DescriptorBase *)p;
		p += base->length;

		if(base->descriptorType == kDescriptorInterface) {
			auto desc = (InterfaceDescriptor *)base;
			assert(desc->length == sizeof(InterfaceDescriptor));

			printf("Interface:\n");
			printf("   if num:%d \n", desc->interfaceNumber);	
			printf("   alternate setting:%d \n", desc->alternateSetting);	
			printf("   num endpoints:%d \n", desc->numEndpoints);	
			printf("   if class:%d \n", desc->interfaceClass);	
			printf("   if sub class:%d \n", desc->interfaceSubClass);	
			printf("   if protocoll:%d \n", desc->interfaceProtocoll);	
			printf("   if id:%d \n", desc->iInterface);	
		}else if(base->descriptorType == kDescriptorEndpoint) {
			auto desc = (EndpointDescriptor *)base;
			assert(desc->length == sizeof(EndpointDescriptor));

			printf("Endpoint:\n");
			printf("   endpoint address:%d \n", desc->endpointAddress);	
			printf("   attributes:%d \n", desc->attributes);	
			printf("   max packet size:%d \n", desc->maxPacketSize);	
			printf("   interval:%d \n", desc->interval);

			int endpoint = desc->endpointAddress & 0x0F;
			device->slots[endpoint] = std::make_unique<Slot>();
			device->slots[endpoint]->maxPacketSize = desc->maxPacketSize;
			device->slots[endpoint]->queue = std::make_unique<QueueEntity>();
			controller->activateEntity(device->slots[endpoint]->queue.get());
		}else if(base->descriptorType == kDescriptorHid) {
			auto desc = (HidDescriptor *)base;
			assert(desc->length == sizeof(HidDescriptor) + (desc->numDescriptors * sizeof(HidDescriptor::Entry)));
			
			printf("HID:\n");
			printf("   hid class:%d \n", desc->hidClass);
			printf("   country code:%d \n", desc->countryCode);
			printf("   num descriptors:%d \n", desc->numDescriptors);
			printf("   Entries:\n");
			for(size_t entry = 0; entry < desc->numDescriptors; entry++) {
				printf("        Entry %lu:\n", entry);
				printf("        length:%d\n", desc->entries[entry].descriptorLength);
				printf("        type:%d\n", desc->entries[entry].descriptorType);
			}
		}else{
			printf("Unexpected descriptor type: %d!\n", base->descriptorType);
		}
	}

	parseReportDescriptor(controller, device);

	// set the device configuration.
	COFIBER_AWAIT WaitForXfer(controller, ControlTransfer(device, 0, kXferToDevice,
			kDestDevice, kStandard, SetupPacket::kSetConfig, 1, 0,
			nullptr, 0));
})

/*// --------------------------------------------------------
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
	
	auto controller = std::make_shared<Controller>(acquire_response.bars(4).address(),
			helx::Irq(irq_handle));
	controller->initialize();

	runHidDevice(controller);
}*/

COFIBER_ROUTINE(cofiber::no_future, bindDevice(mbus::Entity device), ([=] {
	using M = helix::AwaitMechanism;

	auto lane = helix::UniquePipe(COFIBER_AWAIT device.bind());

	// receive the device descriptor.
	uint8_t buffer[128];
	helix::RecvBuffer<M> recv_resp(helix::Dispatcher::global(), lane,
			buffer, 128, 0, 0, kHelResponse);
	COFIBER_AWAIT recv_resp.future();
	HEL_CHECK(recv_resp.error());

	managarm::hw::PciDevice resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());

	// receive the BAR.
	helix::PullDescriptor<M> recv_bar(helix::Dispatcher::global(), lane,
			0, 0, kHelResponse);
	COFIBER_AWAIT recv_bar.future();
	HEL_CHECK(recv_bar.error());
	
	// receive the IRQ.
	helix::PullDescriptor<M> recv_irq(helix::Dispatcher::global(), lane,
			0, 0, kHelResponse);
	COFIBER_AWAIT recv_irq.future();
	HEL_CHECK(recv_irq.error());
	
	// run the UHCI driver.

	assert(resp.bars(0).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(1).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(2).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(3).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(4).io_type() == managarm::hw::IoType::PORT);
	assert(resp.bars(5).io_type() == managarm::hw::IoType::NONE);
	HEL_CHECK(helEnableIo(recv_bar.descriptor().getHandle()));

	auto controller = std::make_shared<Controller>(resp.bars(4).address(),
			helix::UniqueIrq(recv_irq.descriptor()));
	controller->initialize();

	runHidDevice(controller);
}))

COFIBER_ROUTINE(cofiber::no_future, observeDevices(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-class", "0c"),
		mbus::EqualsFilter("pci-subclass", "03"),
		mbus::EqualsFilter("pci-interface", "00")
	});
	auto observer = COFIBER_AWAIT root.linkObserver(std::move(filter),
			[] (mbus::AnyEvent event) {
		if(event.type() == typeid(mbus::AttachEvent)) {
			std::cout << "uhci: Detected device" << std::endl;
			bindDevice(boost::get<mbus::AttachEvent>(event).getEntity());
		}else{
			throw std::runtime_error("Unexpected event type");
		}
	});
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting uhci (usb-)driver\n");

	observeDevices();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}

