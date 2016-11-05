#include <assert.h>
#include <stdio.h>
#include <deque>
#include <experimental/optional>
#include <functional>
#include <iostream>
#include <memory>

#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/memory.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <mbus.hpp>

#include "usb.hpp"
#include "uhci.hpp"
#include "schedule.hpp"
#include <hw.pb.h>

uintptr_t ContiguousPolicy::map(size_t length) {
	assert((length % 0x1000) == 0);

	HelHandle memory;
	void *actual_ptr;
	HEL_CHECK(helAllocateMemory(length, kHelAllocContinuous, &memory));
	HEL_CHECK(helMapMemory(memory, kHelNullHandle, nullptr, 0, length,
			kHelMapReadWrite | kHelMapCopyOnWriteAtFork, &actual_ptr));
	HEL_CHECK(helCloseDescriptor(memory));
	return (uintptr_t)actual_ptr;
}

void ContiguousPolicy::unmap(uintptr_t address, size_t length) {
	HEL_CHECK(helUnmapMemory(kHelNullHandle, (void *)address, length));
}

ContiguousPolicy contiguousPolicy;
ContiguousAllocator contiguousAllocator(contiguousPolicy);

// ----------------------------------------------------------------------------
// QueuedTransaction.
// ----------------------------------------------------------------------------

QueuedTransaction::QueuedTransaction()
	: _completeCounter(0) { }

cofiber::future<void> QueuedTransaction::future() {
	return _promise.get_future();
}

void QueuedTransaction::setupTransfers(TransferDescriptor *transfers, size_t num_transfers) {
	_transfers = transfers;
	_numTransfers = num_transfers;
}

QueueHead::LinkPointer QueuedTransaction::head() {
	return QueueHead::LinkPointer::from(&_transfers[0]);
}

void QueuedTransaction::dumpTransfer() {
	for(size_t i = 0; i < _numTransfers; i++) {
		 printf("    TD %lu:", i);
		_transfers[i].dumpStatus();
		printf("\n");
	}
}

bool QueuedTransaction::progress() {
	while(_completeCounter < _numTransfers) {
		TransferDescriptor *transfer = &_transfers[_completeCounter];
		if(transfer->_controlStatus.isActive())
			return false;

		if(transfer->_controlStatus.isAnyError()) {
			printf("Transfer error!\n");
			dumpTransfer();
			return true;
		}
		
		_completeCounter++;
	}

	printf("Transfer complete!\n");
	_promise.set_value();
	return true;
}

// ----------------------------------------------------------------------------
// ControlTransaction.
// ----------------------------------------------------------------------------

ControlTransaction::ControlTransaction(SetupPacket setup, void *buffer, int address,
		int endpoint, size_t packet_size, XferFlags flags)
: _setup(setup) {
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
// NormalTransaction.
// ----------------------------------------------------------------------------

NormalTransaction::NormalTransaction(void *buffer, size_t length, int address,
		int endpoint, size_t packet_size, XferFlags flags) {
	assert((flags & kXferToDevice) || (flags & kXferToHost));

	size_t data_packets = (length + packet_size - 1) / packet_size;
	size_t desc_size = data_packets * sizeof(TransferDescriptor);
	auto transfers = (TransferDescriptor *)contiguousAllocator.allocate(desc_size);

	size_t progress = 0;
	for(size_t i = 0; i < data_packets; i++) {
		size_t chunk = std::min(packet_size, length - progress);
		new (&transfers[i]) TransferDescriptor(TransferStatus(true, false, false),
			TransferToken(flags & kXferToDevice ? TransferToken::kPacketOut : TransferToken::kPacketIn,
					i % 2 == 0 ? TransferToken::kData0 : TransferToken::kData1,
					address, endpoint, chunk),
			TransferBufferPointer::from((char *)buffer + progress));

		if(i + 1 < data_packets)
			transfers[i]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[i + 1]);
		progress += chunk;
	}

	setupTransfers(transfers, data_packets);
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

// ----------------------------------------------------------------------------
// Endpoint &Endpoint & DeviceState.
// ----------------------------------------------------------------------------

struct EndpointState {
	size_t maxPacketSize;
	std::unique_ptr<QueueEntity> queue;
};

struct DeviceState {
	uint8_t address;
	std::unique_ptr<EndpointState> endpointStates[32];
};

// ----------------------------------------------------------------------------
// Control & InterruptTransfer.
// ----------------------------------------------------------------------------

// arg0 = wValue in the USB spec
// arg1 = wIndex in the USB spec

ControlTransfer::ControlTransfer(XferFlags flags, ControlRecipient recipient,
		ControlType type, uint8_t request, uint16_t arg0, uint16_t arg1,
		void *buffer, size_t length)
	: flags(flags), recipient(recipient), type(type), request(request), arg0(arg0),
			arg1(arg1), buffer(buffer), length(length) { }

InterruptTransfer::InterruptTransfer(void *buffer, size_t length)
	: buffer(buffer), length(length) { }

// ----------------------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------------------

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

	cofiber::future<void> transfer(std::shared_ptr<DeviceState> device_state,
			int endpoint,  ControlTransfer info) {
		assert((info.flags & kXferToDevice) || (info.flags & kXferToHost));
		auto endpoint_state = device_state->endpointStates[endpoint].get();

		SetupPacket setup(info.flags & kXferToDevice ? kDirToDevice : kDirToHost,
				info.recipient, info.type, info.request,
				info.arg0, info.arg1, info.length);
		auto transaction = new ControlTransaction(setup, info.buffer,
				device_state->address, endpoint, endpoint_state->maxPacketSize,
				info.flags);

		if(endpoint_state->queue->transactionList.empty())
			endpoint_state->queue->_queue->_elementPointer = transaction->head();
		endpoint_state->queue->transactionList.push_back(*transaction);

		return transaction->future();
	}

	cofiber::future<void> transfer(std::shared_ptr<DeviceState> device_state,
			int endpoint, XferFlags flags, InterruptTransfer info) {
		assert((flags & kXferToDevice) || (flags & kXferToHost));
		auto endpoint_state = device_state->endpointStates[endpoint].get();

		auto transaction = new NormalTransaction(info.buffer, info.length,
				device_state->address, endpoint, endpoint_state->maxPacketSize,
				flags);

		if(endpoint_state->queue->transactionList.empty())
			endpoint_state->queue->_queue->_elementPointer = transaction->head();
		endpoint_state->queue->transactionList.push_back(*transaction);

		return transaction->future();
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
				++it;
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

// ----------------------------------------------------------------------------
// Device.
// ----------------------------------------------------------------------------

Device::Device(std::shared_ptr<Controller> controller, std::shared_ptr<DeviceState> device_state)
: _controller(std::move(controller)), _deviceState(std::move(device_state)) { }

COFIBER_ROUTINE(cofiber::future<std::string>, Device::configurationDescriptor() const, ([=] {
	auto config = (ConfigDescriptor *)contiguousAllocator.allocate(sizeof(ConfigDescriptor));
	COFIBER_AWAIT _controller->transfer(_deviceState, 0, ControlTransfer(kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorConfig << 8, 0,
			config, sizeof(ConfigDescriptor)));
	assert(config->length == sizeof(ConfigDescriptor));

	printf("Configuration value: %d\n", config->configValue);

	auto buffer = (char *)contiguousAllocator.allocate(config->totalLength);
	COFIBER_AWAIT _controller->transfer(_deviceState, 0, ControlTransfer(kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorConfig << 8, 0,
			buffer, config->totalLength));

	std::string copy(buffer, config->totalLength);
	contiguousAllocator.free(buffer);
	COFIBER_RETURN(std::move(copy));
}))

cofiber::future<void> Device::transfer(ControlTransfer info) const {
	return _controller->transfer(_deviceState, 0, info);
}

COFIBER_ROUTINE(cofiber::future<Configuration>, 
		Device::useConfiguration(int number) const, ([=] {
	// set the device configuration.
	COFIBER_AWAIT _controller->transfer(_deviceState, 0, ControlTransfer(kXferToDevice,
			kDestDevice, kStandard, SetupPacket::kSetConfig, number, 0,
			nullptr, 0));
	COFIBER_RETURN(Configuration(_controller, _deviceState));
}))

// ----------------------------------------------------------------------------
// Configuration.
// ----------------------------------------------------------------------------

Configuration::Configuration(std::shared_ptr<Controller> controller,
		std::shared_ptr<DeviceState> device_state)
: _controller(std::move(controller)), _deviceState(std::move(device_state)) { }

COFIBER_ROUTINE(cofiber::future<Interface>, Configuration::useInterface(int number,
		int alternative) const, ([=] {
	int endpoint = 1 & 0x0F;
	_deviceState->endpointStates[endpoint] = std::make_unique<EndpointState>();
	_deviceState->endpointStates[endpoint]->maxPacketSize = 4;
	_deviceState->endpointStates[endpoint]->queue = std::make_unique<QueueEntity>();
	_controller->activateEntity(_deviceState->endpointStates[endpoint]->queue.get());

	COFIBER_RETURN(Interface(_controller, _deviceState));
}))

// ----------------------------------------------------------------------------
// Interface.
// ----------------------------------------------------------------------------

Interface::Interface(std::shared_ptr<Controller> controller,
		std::shared_ptr<DeviceState> device_state)
: _controller(std::move(controller)), _deviceState(std::move(device_state)) { }

Endpoint Interface::getEndpoint(PipeType type, int number) {
	return Endpoint(_controller, _deviceState, type, number);
}

// ----------------------------------------------------------------------------
// Endpoint.
// ----------------------------------------------------------------------------

Endpoint::Endpoint(std::shared_ptr<Controller> controller, 
		std::shared_ptr<DeviceState> device_state, PipeType type, int number)
: _controller(std::move(controller)), _deviceState(std::move(device_state)),
		_type(type), _number(number) { }

cofiber::future<void> Endpoint::transfer(InterruptTransfer info) {
	XferFlags flag = kXferToDevice;
	if(_type == PipeType::in)
		flag = kXferToHost;

	return _controller->transfer(_deviceState, _number, flag, info);
}

cofiber::no_future runHidDevice(Device device);

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
	
	auto device_state = std::make_shared<DeviceState>();
	device_state->address = 0;
	device_state->endpointStates[0] = std::make_unique<EndpointState>();
	device_state->endpointStates[0]->maxPacketSize = 8;
	device_state->endpointStates[0]->queue = std::make_unique<QueueEntity>();
	controller->activateEntity(device_state->endpointStates[0]->queue.get());

	// set the device_state address.
	COFIBER_AWAIT controller->transfer(device_state, 0, ControlTransfer(kXferToDevice,
			kDestDevice, kStandard, SetupPacket::kSetAddress, 1, 0,
			nullptr, 0));
	device_state->address = 1;

	// enquire the maximum packet size of endpoint 0 and get the device_state descriptor.
	auto descriptor = (DeviceDescriptor *)contiguousAllocator.allocate(sizeof(DeviceDescriptor));
	COFIBER_AWAIT controller->transfer(device_state, 0, ControlTransfer(kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorDevice << 8, 0,
			descriptor, 8));
	device_state->endpointStates[0]->maxPacketSize = descriptor->maxPacketSize;
	
	COFIBER_AWAIT controller->transfer(device_state, 0, ControlTransfer(kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorDevice << 8, 0,
			descriptor, sizeof(DeviceDescriptor)));
	assert(descriptor->length == sizeof(DeviceDescriptor));

	runHidDevice(Device(controller, device_state));
}))

COFIBER_ROUTINE(cofiber::no_future, observeDevices(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-class", "0c"),
		mbus::EqualsFilter("pci-subclass", "03"),
		mbus::EqualsFilter("pci-interface", "00")
	});
	COFIBER_AWAIT root.linkObserver(std::move(filter),
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

