#include <assert.h>
#include <stdio.h>
#include <deque>
#include <experimental/optional>
#include <functional>
#include <iostream>
#include <memory>

#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <cofiber.hpp>
#include <frigg/atomic.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <frigg/memory.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/server.hpp>

#include "uhci.hpp"
#include "schedule.hpp"

std::vector<std::shared_ptr<Controller>> globalControllers;

// ----------------------------------------------------------------------------
// Memory management.
// ----------------------------------------------------------------------------

template<typename T, typename... Args>
contiguous_ptr<T> make_contiguous(Args &&... args) {
	// TODO: Delete p on exception.
	auto p = contiguousAllocator.allocate(sizeof(T));
	return contiguous_ptr<T>{new (p) QueueHead{std::forward<Args>(args)...}};
}

// ----------------------------------------------------------------------------
// Pointer.
// ----------------------------------------------------------------------------

Pointer Pointer::from(TransferDescriptor *item) {
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(item, &physical));
	assert(physical % sizeof(*item) == 0);
	assert((physical & 0xFFFFFFFF) == physical);
	return Pointer(physical, false);
}
Pointer Pointer::from(QueueHead *item) {
	uintptr_t physical;
	HEL_CHECK(helPointerPhysical(item, &physical));
	assert(physical % sizeof(*item) == 0);
	assert((physical & 0xFFFFFFFF) == physical);
	return Pointer(physical, true);
}

// ----------------------------------------------------------------
// DeviceState
// ----------------------------------------------------------------

DeviceState::DeviceState(std::shared_ptr<Controller> controller, int device)
: _controller{std::move(controller)}, _device(device) { }

async::result<std::string> DeviceState::configurationDescriptor() {
	return _controller->configurationDescriptor(_device);
}

COFIBER_ROUTINE(async::result<Configuration>, 
		DeviceState::useConfiguration(int number), ([=] {
	COFIBER_AWAIT _controller->useConfiguration(_device, number);
	COFIBER_RETURN(Configuration{std::make_shared<ConfigurationState>(_controller,
			_device, number)});
}))

async::result<void> DeviceState::transfer(ControlTransfer info) {
	return _controller->transfer(_device, 0, info);
}

// ----------------------------------------------------------------------------
// ConfigurationState
// ----------------------------------------------------------------------------

ConfigurationState::ConfigurationState(std::shared_ptr<Controller> controller,
		int device, int configuration)
: _controller{std::move(controller)}, _device(device), _configuration(configuration) { }

COFIBER_ROUTINE(async::result<Interface>, ConfigurationState::useInterface(int number,
		int alternative), ([=] {
	COFIBER_AWAIT _controller->useInterface(_device, number, alternative);
	COFIBER_RETURN(Interface{std::make_shared<InterfaceState>(_controller, _device, number)});
}))

// ----------------------------------------------------------------------------
// InterfaceState
// ----------------------------------------------------------------------------

InterfaceState::InterfaceState(std::shared_ptr<Controller> controller,
		int device, int interface)
: _controller{std::move(controller)}, _device(device), _interface(interface) { }

COFIBER_ROUTINE(async::result<Endpoint>, InterfaceState::getEndpoint(PipeType type,
		int number), ([=] {
	COFIBER_RETURN(Endpoint{std::make_shared<EndpointState>(_controller,
			_device, type, number)});
}))

// ----------------------------------------------------------------------------
// EndpointState
// ----------------------------------------------------------------------------

EndpointState::EndpointState(std::shared_ptr<Controller> controller,
		int device, PipeType type, int endpoint)
: _controller{std::move(controller)}, _device(device), _type(type), _endpoint(endpoint) { }

async::result<void> EndpointState::transfer(ControlTransfer info) {
	(void)info;
	assert(!"FIXME: Implement this");
	__builtin_unreachable();
}

async::result<void> EndpointState::transfer(InterruptTransfer info) {
	return _controller->transfer(_device, _type, _endpoint, info);
}

async::result<void> EndpointState::transfer(BulkTransfer info) {
	return _controller->transfer(_device, _type, _endpoint, info);
}

// ----------------------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------------------

Controller::Controller(uint16_t base, helix::UniqueIrq irq)
: _base(base), _irq(frigg::move(irq)),
		_lastFrame(0), _frameCounter(0) {
	for(int i = 1; i < 128; i++) {
		_addressStack.push(i);
	}
}

void Controller::initialize() {
	auto initial_status = frigg::readIo<uint16_t>(_base + kRegStatus);
	assert(!(initial_status & kStatusInterrupt));
	assert(!(initial_status & kStatusError));
	
	// host controller reset.
	frigg::writeIo<uint16_t>(_base + kRegCommand, 0x02);
	while((frigg::readIo<uint16_t>(_base + kRegCommand) & 0x02) != 0) { }

	// setup the frame list.
	HelHandle list_handle;
	HEL_CHECK(helAllocateMemory(4096, 0, &list_handle));
	void *list_mapping;
	HEL_CHECK(helMapMemory(list_handle, kHelNullHandle,
			nullptr, 0, 4096, kHelMapReadWrite, &list_mapping));
	
	auto list_pointer = (FrameList *)list_mapping;
	for(int i = 0; i < 1024; i++) {
		_periodicQh[i]._linkPointer = Pointer::from(&_asyncQh);
		list_pointer->entries[i] = FrameListPointer::from(&_periodicQh[i]);
	}

	// pass the frame list to the controller and run it.
	uintptr_t list_physical;
	HEL_CHECK(helPointerPhysical(list_pointer, &list_physical));
	assert((list_physical % 0x1000) == 0);
	frigg::writeIo<uint32_t>(_base + kRegFrameListBaseAddr, list_physical);
	frigg::writeIo<uint16_t>(_base + kRegCommand, 0x01);
	
	// enable interrupts.
	frigg::writeIo<uint16_t>(_base + kRegInterruptEnable, 0x0F);

	pollDevices();
	handleIrqs();
}

COFIBER_ROUTINE(cofiber::no_future, Controller::handleIrqs(), ([=] {
	while(true) {
		helix::AwaitIrq await_irq;
		auto &&submit = helix::submitAwaitIrq(_irq, &await_irq, helix::Dispatcher::global());
		COFIBER_AWAIT submit.async_wait();
		HEL_CHECK(await_irq.error());

		_updateFrame();

		auto status = frigg::readIo<uint16_t>(_base + kRegStatus);
		assert(!(status & 0x10));
		assert(!(status & 0x08));
		if(!(status & (kStatusInterrupt | kStatusError)))
			continue;

		if(status & kStatusError)
			printf("uhci: Error interrupt\n");
		frigg::writeIo<uint16_t>(_base + kRegStatus, kStatusInterrupt | kStatusError);
		
		//printf("uhci: Processing transfers.\n");
		_progressSchedule();
	}
}))

void Controller::_updateFrame() {
	auto frame = frigg::readIo<uint16_t>(_base + kRegFrameNumber);
	auto counter = (frame > _lastFrame) ? (_frameCounter + frame - _lastFrame)
			: (_frameCounter + 2048 - _lastFrame + frame);

	if(counter / 1024 > _frameCounter / 1024)
		_pollDoorbell.ring();

	_lastFrame = frame;
	_frameCounter = counter;

	// This is where we perform actual reclamation.
	while(!_reclaimQueue.empty()) {
		auto item = &_reclaimQueue.front();
		if(item->reclaimFrame > _frameCounter)
			break;
		_reclaimQueue.pop_front();
		delete item;
	}
}

// ----------------------------------------------------------------
// Controller: USB device discovery methods.
// ----------------------------------------------------------------

COFIBER_ROUTINE(async::result<void>, Controller::pollDevices(), ([=] {
	while(true) {
		for(int i = 0; i < 2; i++) {
			auto port_register = kRegPort1StatusControl + 2 * i;

			// poll for connect status change and immediately reset that bit.
			if(!(frigg::readIo<uint16_t>(_base + port_register) & kRootConnectChange))
				continue;
			frigg::writeIo<uint16_t>(_base + port_register, kRootConnectChange);

			// TODO: delete current device.
			
			// check if a new device was attached to the port.
			auto port_status = frigg::readIo<uint16_t>(_base + port_register);
			assert(!(port_status & kRootEnabled));
			if(!(port_status & kRootConnected))
				continue;

			std::cout << "uhci: USB device connected" << std::endl;

			// reset the port for 50ms.
			frigg::writeIo<uint16_t>(_base + port_register, kRootReset);
		
			// TODO: do not busy-wait.
			uint64_t start;
			HEL_CHECK(helGetClock(&start));
			while(true) {
				uint64_t ticks;
				HEL_CHECK(helGetClock(&ticks));
				if(ticks - start >= 50000000)
					break;
			}

			// enable the port and wait until it is available.
			frigg::writeIo<uint16_t>(_base + port_register, kRootEnabled);
			while(true) {
				port_status = frigg::readIo<uint16_t>(_base + port_register);
				if((port_status & kRootEnabled))
					break;
			}

			// disable the port if there was a concurrent disconnect.
			if(port_status & kRootConnectChange) {
				std::cout << "uhci: Disconnect during device enumeration." << std::endl;
				frigg::writeIo<uint16_t>(_base + port_register, 0);
				continue;
			}
		
			COFIBER_AWAIT probeDevice();
		}

		COFIBER_AWAIT _pollDoorbell.async_wait();
	}
}))

COFIBER_ROUTINE(async::result<void>, Controller::probeDevice(), ([=] {
	// This queue will become the default control pipe of our new device.
	auto queue = new QueueEntity;
	_linkAsync(queue);

	// Allocate an address for the device.
	assert(!_addressStack.empty());
	auto address = _addressStack.front();
	_addressStack.pop();

	COFIBER_AWAIT _directTransfer(0, 0, ControlTransfer{kXferToDevice,
			kDestDevice, kStandard, SetupPacket::kSetAddress,
			static_cast<uint16_t>(address), 0, nullptr, 0}, queue, 8);

	// Enquire the maximum packet size of the default control pipe.
	auto descriptor = (DeviceDescriptor *)contiguousAllocator.allocate(sizeof(DeviceDescriptor));
	COFIBER_AWAIT _directTransfer(address, 0, ControlTransfer{kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorDevice << 8, 0,
			descriptor, 8}, queue, 8);

	_activeDevices[address].controlStates[0].queueEntity = queue;
	_activeDevices[address].controlStates[0].maxPacketSize = descriptor->maxPacketSize;

	// Now the device is set up.
	COFIBER_AWAIT transfer(address, 0, ControlTransfer{kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorDevice << 8, 0,
			descriptor, sizeof(DeviceDescriptor)});
	assert(descriptor->length == sizeof(DeviceDescriptor));

	// TODO: Read configuration descriptor from the device.

	char class_code[3], sub_class[3], protocol[3];
	char vendor[5], product[5], release[5];
	sprintf(class_code, "%.2x", descriptor->deviceClass);
	sprintf(sub_class, "%.2x", descriptor->deviceSubclass);
	sprintf(protocol, "%.2x", descriptor->deviceProtocol);
	sprintf(vendor, "%.4x", descriptor->idVendor);
	sprintf(product, "%.4x", descriptor->idProduct);
	sprintf(release, "%.4x", descriptor->bcdDevice);

	std::unordered_map<std::string, std::string> mbus_desc {
		{ "usb.type", "device" },
		{ "usb.vendor", vendor },
		{ "usb.product", product },
		{ "usb.class", class_code },
		{ "usb.subclass", sub_class },
		{ "usb.protocol", protocol },
		{ "usb.release", release }
	};
	
	std::cout << "class_code: " << class_code << std::endl;

	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
	
	char name[3];
	sprintf(name, "%.2x", address);
	auto object = COFIBER_AWAIT root.createObject(name, mbus_desc,
			[=] (mbus::AnyQuery query) -> async::result<helix::UniqueDescriptor> {
		(void)query;

		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		auto state = std::make_shared<DeviceState>(shared_from_this(), address);
		protocols::usb::serve(Device{std::move(state)}, std::move(local_lane));

		async::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.async_get();
	});
	std::cout << "Created object " << name << std::endl;

	COFIBER_RETURN();
}))

// ------------------------------------------------------------------------
// Controller: Device management.
// ------------------------------------------------------------------------

COFIBER_ROUTINE(async::result<std::string>, Controller::configurationDescriptor(int address),
		([=] {
	auto header = (ConfigDescriptor *)contiguousAllocator.allocate(sizeof(ConfigDescriptor));
	COFIBER_AWAIT transfer(address, 0, ControlTransfer{kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorConfig << 8, 0,
			header, sizeof(ConfigDescriptor)});
	assert(header->length == sizeof(ConfigDescriptor));

	//printf("Configuration value: %d\n", config->configValue);

	auto descriptor = (char *)contiguousAllocator.allocate(header->totalLength);
	COFIBER_AWAIT transfer(address, 0, ControlTransfer{kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorConfig << 8, 0,
			descriptor, header->totalLength});

	std::string copy(descriptor, header->totalLength);
	contiguousAllocator.free(header);
	contiguousAllocator.free(descriptor);
	COFIBER_RETURN(std::move(copy));
}))

COFIBER_ROUTINE(async::result<void>, Controller::useConfiguration(int address,
		int configuration), ([=] {
	COFIBER_AWAIT transfer(address, 0, ControlTransfer{kXferToDevice,
			kDestDevice, kStandard, SetupPacket::kSetConfig,
			static_cast<uint16_t>(configuration), 0, nullptr, 0});
	
	COFIBER_RETURN();
}))

COFIBER_ROUTINE(async::result<void>, Controller::useInterface(int address,
		int interface, int alternative), ([=] {
	auto descriptor = COFIBER_AWAIT configurationDescriptor(address);
	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		(void)length;

		if(type != kDescriptorEndpoint)
			return;
		auto desc = (EndpointDescriptor *)p;

		// TODO: Pay attention to interface/alternative.
		
		int pipe = info.endpointNumber.value();
		if(info.endpointIn.value()) {
			_activeDevices[address].inStates[pipe].maxPacketSize = desc->maxPacketSize;
			_activeDevices[address].inStates[pipe].queueEntity = new QueueEntity;
			this->_linkAsync(_activeDevices[address].inStates[pipe].queueEntity);
		}else{
			_activeDevices[address].outStates[pipe].maxPacketSize = desc->maxPacketSize;
			_activeDevices[address].outStates[pipe].queueEntity = new QueueEntity;
			this->_linkAsync(_activeDevices[address].outStates[pipe].queueEntity);
		}
	});
	
	COFIBER_RETURN();
}))

// ------------------------------------------------------------------------
// Controller: Transfer functions.
// ------------------------------------------------------------------------

async::result<void> Controller::transfer(int address, int pipe, ControlTransfer info) {
	auto device = &_activeDevices[address];
	auto endpoint = &device->controlStates[pipe];
	
	assert((info.flags == kXferToDevice) || (info.flags == kXferToHost));
	// TODO: Pass the setup packet into this function; do not allocate it here.
	// TODO: Ensure the packet size fits into 16 bits.
	auto setup = (SetupPacket *)contiguousAllocator.allocate(sizeof(SetupPacket));
	new (setup) SetupPacket{info.flags == kXferToDevice ? kDirToDevice : kDirToHost,
			info.recipient, info.type, info.request, info.arg0, info.arg1,
			static_cast<uint16_t>(info.length)};

	auto transaction = _buildControl(address, pipe, info.flags,
			setup, info.buffer, info.length, endpoint->maxPacketSize);
	_linkTransaction(endpoint->queueEntity, transaction);
	return transaction->promise.async_get();
}

async::result<void> Controller::transfer(int address, PipeType type, int pipe,
		InterruptTransfer info) {
	// TODO: Ensure pipe type matches transfer direction.
	auto device = &_activeDevices[address];
	EndpointSlot *endpoint;
	if(type == PipeType::in) {
		endpoint = &device->inStates[pipe];
	}else{
		assert(type == PipeType::out);
		endpoint = &device->outStates[pipe];
	}

	auto transaction = _buildInterruptOrBulk(address, pipe, info.flags,
			info.buffer, info.length, endpoint->maxPacketSize);
	_linkTransaction(endpoint->queueEntity, transaction);
	return transaction->promise.async_get();
}

async::result<void> Controller::transfer(int address, PipeType type, int pipe,
		BulkTransfer info) {
	// TODO: Ensure pipe type matches transfer direction.
	auto device = &_activeDevices[address];
	EndpointSlot *endpoint;
	if(type == PipeType::in) {
		endpoint = &device->inStates[pipe];
	}else{
		assert(type == PipeType::out);
		endpoint = &device->outStates[pipe];
	}

	auto transaction = _buildInterruptOrBulk(address, pipe, info.flags,
			info.buffer, info.length, endpoint->maxPacketSize);
	_linkTransaction(endpoint->queueEntity, transaction);
	return transaction->promise.async_get();
}

auto Controller::_buildControl(int address, int pipe, XferFlags dir,
		SetupPacket *setup, void *buffer, size_t length, size_t max_packet_size) -> Transaction * {
	assert((dir == kXferToDevice) || (dir == kXferToHost));

	size_t data_packets = (length + max_packet_size - 1) / max_packet_size;
	size_t desc_size = (data_packets + 2) * sizeof(TransferDescriptor);
	auto transfers = (TransferDescriptor *)contiguousAllocator.allocate(desc_size);

	new (&transfers[0]) TransferDescriptor(td_status::active(true)
			| td_status::detectShort(true),
			td_token::pid(Packet::setup) | td_token::address(address) | td_token::pipe(pipe)
			| td_token::length(sizeof(SetupPacket) - 1),
			TransferBufferPointer::from(setup));
	transfers[0]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[1]);

	size_t progress = 0;
	for(size_t i = 0; i < data_packets; i++) {
		size_t chunk = std::min(max_packet_size, length - progress);
		assert(chunk);
		new (&transfers[i + 1]) TransferDescriptor(td_status::active(true)
			| td_status::detectShort(true),
			td_token::pid(dir == kXferToDevice ? Packet::out : Packet::in)
			| td_token::toggle(i % 2 != 0) | td_token::address(address) | td_token::pipe(pipe)
			| td_token::length(chunk - 1),
			TransferBufferPointer::from((char *)buffer + progress));
		transfers[i + 1]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[i + 2]);
		progress += chunk;
	}

	new (&transfers[data_packets + 1]) TransferDescriptor(td_status::active(true)
			| td_status::completionIrq(true),
			td_token::pid(dir == kXferToDevice ? Packet::in : Packet::out)
			| td_token::address(address) | td_token::pipe(pipe)
			| td_token::length(0x7FF),
			TransferBufferPointer());

	return new Transaction{transfers, data_packets + 2};
}

auto Controller::_buildInterruptOrBulk(int address, int pipe, XferFlags dir,
		void *buffer, size_t length, size_t max_packet_size) -> Transaction * {
	assert((dir == kXferToDevice) || (dir == kXferToHost));

	size_t data_packets = (length + max_packet_size - 1) / max_packet_size;
	size_t desc_size = data_packets * sizeof(TransferDescriptor);
	auto transfers = (TransferDescriptor *)contiguousAllocator.allocate(desc_size);

	size_t progress = 0;
	for(size_t i = 0; i < data_packets; i++) {
		size_t chunk = std::min(max_packet_size, length - progress);
		assert(chunk);
		new (&transfers[i]) TransferDescriptor(td_status::active(true)
			| td_status::completionIrq(i + 1 == data_packets) | td_status::detectShort(true),
			td_token::pid(dir == kXferToDevice ? Packet::out : Packet::in)
			| td_token::toggle(i % 2 != 0) | td_token::address(address) | td_token::pipe(pipe)
			| td_token::length(chunk - 1),
			TransferBufferPointer::from((char *)buffer + progress));

		if(i + 1 < data_packets)
			transfers[i]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[i + 1]);
		progress += chunk;
	}

	return new Transaction{transfers, data_packets};
}

async::result<void> Controller::_directTransfer(int address, int pipe, ControlTransfer info,
		QueueEntity *queue, size_t max_packet_size) {
	assert((info.flags == kXferToDevice) || (info.flags == kXferToHost));
	// TODO: Pass the setup packet into this function; do not allocate it here.
	// TODO: Ensure the packet size fits into 16 bits.
	auto setup = (SetupPacket *)contiguousAllocator.allocate(sizeof(SetupPacket));
	new (setup) SetupPacket{info.flags == kXferToDevice ? kDirToDevice : kDirToHost,
			info.recipient, info.type, info.request, info.arg0, info.arg1,
			static_cast<uint16_t>(info.length)};

	auto transaction = _buildControl(address, pipe, info.flags,
			setup, info.buffer, info.length, max_packet_size);
	_linkTransaction(queue, transaction);
	return transaction->promise.async_get();
}

// ----------------------------------------------------------------
// Controller: Schedule manipulation functions.
// ----------------------------------------------------------------

void Controller::_linkAsync(QueueEntity *entity) {
	if(_asyncSchedule.empty()) {
		_asyncQh._linkPointer = QueueHead::LinkPointer::from(entity->head.get());
	}else{
		_asyncSchedule.back().head->_linkPointer
				= QueueHead::LinkPointer::from(entity->head.get());
	}
	_asyncSchedule.push_back(*entity);
}

void Controller::_linkTransaction(QueueEntity *queue, Transaction *transaction) {
	if(queue->transactions.empty())
		queue->head->_elementPointer = QueueHead::LinkPointer::from(&transaction->transfers[0]);
	queue->transactions.push_back(*transaction);
}

void Controller::_progressSchedule() {
	auto it = _asyncSchedule.begin();
	while(it != _asyncSchedule.end()) {
		_progressQueue(&(*it));
		++it;
	}
}

void Controller::_progressQueue(QueueEntity *entity) {
	if(entity->transactions.empty())
		return;

	auto active = &entity->transactions.front();
	while(active->numComplete < active->numTransfers) {
		auto &transfer = active->transfers[active->numComplete];
		if((transfer.status.load() & td_status::active)
				|| (transfer.status.load() & td_status::errorBits))
			break;

		active->numComplete++;
	}
	
	if(active->numComplete == active->numTransfers) {
		//printf("Transfer complete!\n");
		active->promise.set_value();

		// Clean up the Queue.
		entity->transactions.pop_front();
		_reclaim(active);
		
		// Schedule the next transaction.
		assert(entity->head->_elementPointer.isTerminate());
		if(!entity->transactions.empty()) {
			auto front = &entity->transactions.front();
			entity->head->_elementPointer = QueueHead::LinkPointer::from(&front->transfers[0]);
		}
	}else if(active->transfers[active->numComplete].status.load() & td_status::errorBits) {
		printf("Transfer error!\n");
		_dump(active);
		
		// Clean up the Queue.
		entity->transactions.pop_front();
		_reclaim(active);
	}
}

void Controller::_reclaim(ScheduleItem *item) {
	assert(item->reclaimFrame == -1);

	_updateFrame();
	item->reclaimFrame = _frameCounter + 1;
	_reclaimQueue.push_back(*item);
}

// ----------------------------------------------------------------------------
// Debugging functions.
// ----------------------------------------------------------------------------

void Controller::_dump(Transaction *transaction) {
	for(size_t i = 0; i < transaction->numTransfers; i++) {
		printf("    TD %lu:", i);
		transaction->transfers[i].dumpStatus();
		printf("\n");
	}
}

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, bindController(mbus::Entity entity), ([=] {
	protocols::hw::Device device(COFIBER_AWAIT entity.bind());
	auto info = COFIBER_AWAIT device.getPciInfo();
	assert(info.barInfo[4].ioType == protocols::hw::IoType::kIoTypePort);
	auto bar = COFIBER_AWAIT device.accessBar(4);
	auto irq = COFIBER_AWAIT device.accessIrq();

	// TODO: Disable the legacy support registers of all UHCI devices
	// before using one of them!
	auto legsup = COFIBER_AWAIT device.loadPciSpace(kPciLegacySupport);
	std::cout << "UHCI: Legacy support register: " << legsup << std::endl;

	HEL_CHECK(helEnableIo(bar.getHandle()));

	auto controller = std::make_shared<Controller>(info.barInfo[4].address, std::move(irq));
	controller->initialize();

	globalControllers.push_back(std::move(controller));
}))

COFIBER_ROUTINE(cofiber::no_future, observeControllers(), ([] {
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
			bindController(boost::get<mbus::AttachEvent>(event).getEntity());
		}else{
			throw std::runtime_error("Unexpected event type");
		}
	});
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting UHCI driver\n");

	observeControllers();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}

