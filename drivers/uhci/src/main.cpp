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
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/server.hpp>

#include "uhci.hpp"
#include "schedule.hpp"
#include <hw.pb.h>

std::vector<std::shared_ptr<Controller>> globalControllers;

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

new (&transfers[0]) TransferDescriptor(TransferStatus(true, true, false, false),
		TransferToken(TransferToken::kPacketSetup, TransferToken::kData0,
				address, endpoint, sizeof(SetupPacket)),
		TransferBufferPointer::from(&_setup));
transfers[0]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[1]);

size_t progress = 0;
for(size_t i = 0; i < data_packets; i++) {
	size_t chunk = std::min(packet_size, _setup.wLength - progress);
	new (&transfers[i + 1]) TransferDescriptor(TransferStatus(true, true, false, false),
		TransferToken(flags & kXferToDevice ? TransferToken::kPacketOut : TransferToken::kPacketIn,
				i % 2 == 0 ? TransferToken::kData0 : TransferToken::kData1,
				address, endpoint, chunk),
		TransferBufferPointer::from((char *)buffer + progress));
	transfers[i + 1]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[i + 2]);
	progress += chunk;
}

new (&transfers[data_packets + 1]) TransferDescriptor(TransferStatus(true, true, false, false),
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
	new (&transfers[i]) TransferDescriptor(TransferStatus(true, true, false, false),
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

// ----------------------------------------------------------------------------
// DummyEntity.
// ----------------------------------------------------------------------------

DummyEntity::DummyEntity() {
	_transfer = (TransferDescriptor *)contiguousAllocator.allocate(sizeof(TransferDescriptor));

	new (_transfer) TransferDescriptor(TransferStatus(false, true, false, false),
			TransferToken(TransferToken::PacketId::kPacketIn, 
			TransferToken::DataToggle::kData0, 0, 0, 0), TransferBufferPointer());
	_transfer->_linkPointer = QueueHead::LinkPointer();
}

QueueHead::LinkPointer DummyEntity::head() {
	return QueueHead::LinkPointer::from(_transfer);
}

void DummyEntity::linkNext(QueueHead::LinkPointer link) {
	_transfer->_linkPointer = link;
}

// this function does not need to do anything
void DummyEntity::progress() { }

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
> periodicSchedule[1024];

boost::intrusive::list<
	ScheduleEntity,
	boost::intrusive::member_hook<
		ScheduleEntity,
		boost::intrusive::list_member_hook<>,
		&ScheduleEntity::scheduleHook
	>
> asyncSchedule;

// ----------------------------------------------------------------
// DeviceState
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::future<std::string>, DeviceState::configurationDescriptor(), ([=] {
	auto config = (ConfigDescriptor *)contiguousAllocator.allocate(sizeof(ConfigDescriptor));
	COFIBER_AWAIT _controller->transfer(shared_from_this(), 0, ControlTransfer(kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorConfig << 8, 0,
			config, sizeof(ConfigDescriptor)));
	assert(config->length == sizeof(ConfigDescriptor));

	printf("Configuration value: %d\n", config->configValue);

	auto buffer = (char *)contiguousAllocator.allocate(config->totalLength);
	COFIBER_AWAIT _controller->transfer(shared_from_this(), 0, ControlTransfer(kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorConfig << 8, 0,
			buffer, config->totalLength));

	std::string copy(buffer, config->totalLength);
	contiguousAllocator.free(buffer);
	COFIBER_RETURN(std::move(copy));
}))

COFIBER_ROUTINE(cofiber::future<Configuration>, 
		DeviceState::useConfiguration(int number), ([=] {
	// set the device configuration.
	COFIBER_AWAIT _controller->transfer(shared_from_this(), 0, ControlTransfer(kXferToDevice,
			kDestDevice, kStandard, SetupPacket::kSetConfig, number, 0,
			nullptr, 0));
	auto config_state = std::make_shared<ConfigurationState>(shared_from_this());
	COFIBER_RETURN(Configuration(config_state));
}))

cofiber::future<void> DeviceState::transfer(ControlTransfer info) {
	return _controller->transfer(shared_from_this(), 0, info);
}

// ----------------------------------------------------------------------------
// ConfigurationState
// ----------------------------------------------------------------------------

ConfigurationState::ConfigurationState(std::shared_ptr<DeviceState> device)
: _device(std::move(device)) { }

COFIBER_ROUTINE(cofiber::future<Interface>, ConfigurationState::useInterface(int number,
		int alternative), ([=] {
	auto interface = std::make_shared<InterfaceState>(shared_from_this());
	
	auto descriptor = COFIBER_AWAIT Device(_device).configurationDescriptor();
	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		if(type != kDescriptorEndpoint)
			return;
		auto desc = (EndpointDescriptor *)p;	
		
		int endpoint = info.endpointNumber.value();
		_device->endpointStates[endpoint] = std::make_shared<EndpointState>(PipeType::in, endpoint);
		_device->endpointStates[endpoint]->maxPacketSize = desc->maxPacketSize;
		_device->endpointStates[endpoint]->queue = std::make_unique<QueueEntity>();
		_device->endpointStates[endpoint]->_interface = interface;
		_device->_controller->activateAsync(_device->endpointStates[endpoint]->queue.get());
	});
	
	COFIBER_RETURN(Interface(interface));
}))

// ----------------------------------------------------------------------------
// InterfaceState
// ----------------------------------------------------------------------------

InterfaceState::InterfaceState(std::shared_ptr<ConfigurationState> config)
: _config(std::move(config)) { }

COFIBER_ROUTINE(cofiber::future<Endpoint>, InterfaceState::getEndpoint(PipeType type, int number),
		([=] {
	COFIBER_RETURN(Endpoint(_config->_device->endpointStates[number]));
}))

// ----------------------------------------------------------------------------
// EndpointState
// ----------------------------------------------------------------------------

EndpointState::EndpointState(PipeType type, int number)
: _type(type), _number(number) { }

cofiber::future<void> EndpointState::transfer(ControlTransfer info) {
	assert(!"FIXME: Implement this");
}

cofiber::future<void> EndpointState::transfer(InterruptTransfer info) {
	XferFlags flag = kXferToDevice;
	if(_type == PipeType::in)
		flag = kXferToHost;

	return _interface->_config->_device->_controller->transfer(
			_interface->_config->_device, _number, flag, info);
}

// ----------------------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------------------

Controller::Controller(uint16_t base, helix::UniqueIrq irq)
: _base(base), _irq(frigg::move(irq)),
		_lastFrame(0), _lastCounter(0) {
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

	activatePeriodic(0, &_irqDummy);

	pollDevices();

	handleIrqs();
}

COFIBER_ROUTINE(cofiber::future<void>, Controller::pollDevices(), ([=] {
	printf("entered pollDevices\n");
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

	COFIBER_RETURN();
}))

COFIBER_ROUTINE(cofiber::future<void>, Controller::probeDevice(), ([=] {
	printf("entered probeDevice\n");
	auto device_state = std::make_shared<DeviceState>();
	device_state->address = 0;
	device_state->endpointStates[0] = std::make_shared<EndpointState>(PipeType::control, 0);
	device_state->endpointStates[0]->maxPacketSize = 8;
	device_state->endpointStates[0]->queue = std::make_unique<QueueEntity>();
	activateAsync(device_state->endpointStates[0]->queue.get());

	// set the device_state address.
	assert(!_addressStack.empty());
	COFIBER_AWAIT transfer(device_state, 0, ControlTransfer(kXferToDevice,
			kDestDevice, kStandard, SetupPacket::kSetAddress, _addressStack.front(), 0,
			nullptr, 0));
	device_state->address = _addressStack.front();
	_activeDevices[_addressStack.front()] = device_state;
	_addressStack.pop();

	// enquire the maximum packet size of endpoint 0 and get the device descriptor.
	auto descriptor = (DeviceDescriptor *)contiguousAllocator.allocate(sizeof(DeviceDescriptor));
	COFIBER_AWAIT transfer(device_state, 0, ControlTransfer(kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorDevice << 8, 0,
			descriptor, 8));
	device_state->endpointStates[0]->maxPacketSize = descriptor->maxPacketSize;
	
	COFIBER_AWAIT transfer(device_state, 0, ControlTransfer(kXferToHost,
			kDestDevice, kStandard, SetupPacket::kGetDescriptor, kDescriptorDevice << 8, 0,
			descriptor, sizeof(DeviceDescriptor)));
	assert(descriptor->length == sizeof(DeviceDescriptor));

	// TODO:read configuration descriptor from the device

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
	sprintf(name, "%.2x", device_state->address);
	auto object = COFIBER_AWAIT root.createObject(name, mbus_desc,
			[&] (mbus::AnyQuery query) -> cofiber::future<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		protocols::usb::serve(Device(device_state), std::move(local_lane));

		cofiber::promise<helix::UniqueDescriptor> promise;
		promise.set_value(std::move(remote_lane));
		return promise.get_future();
	});
	std::cout << "Created object " << name << std::endl;

	COFIBER_RETURN();
}))

void Controller::activatePeriodic(int frame, ScheduleEntity *entity) {
	if(periodicSchedule[frame].empty()) {
		_periodicQh[frame]._linkPointer = entity->head();
	}else{
		periodicSchedule[frame].back().linkNext(entity->head());
	}
	entity->linkNext(QueueHead::LinkPointer::from(&_asyncQh));
	periodicSchedule[frame].push_back(*entity);
}

void Controller::activateAsync(ScheduleEntity *entity) {
	if(asyncSchedule.empty()) {
		_asyncQh._linkPointer = entity->head();
	}else{
		asyncSchedule.back().linkNext(entity->head());
	}
	asyncSchedule.push_back(*entity);
}

cofiber::future<void> Controller::transfer(std::shared_ptr<DeviceState> device_state,
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

cofiber::future<void> Controller::transfer(std::shared_ptr<DeviceState> device_state,
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

COFIBER_ROUTINE(cofiber::no_future, Controller::handleIrqs(), ([=] {
	while(true) {
		helix::AwaitIrq<helix::AwaitMechanism> await_irq;
		helix::submitAwaitIrq(_irq, &await_irq, helix::Dispatcher::global());
		COFIBER_AWAIT await_irq.future();
		HEL_CHECK(await_irq.error());

		auto status = frigg::readIo<uint16_t>(_base + kRegStatus);
		assert(!(status & 0x10));
		assert(!(status & 0x08));
		if(!(status & (kStatusInterrupt | kStatusError)))
			continue;

		if(status & kStatusError)
			printf("uhci: Error interrupt\n");
		frigg::writeIo<uint16_t>(_base + kRegStatus, kStatusInterrupt | kStatusError);
		
		auto frame = frigg::readIo<uint16_t>(_base + kRegFrameNumber);
		auto counter = (frame > _lastFrame) ? (_lastCounter + frame - _lastFrame)
				: (_lastCounter + 2048 - _lastFrame + frame);
		
		//if(counter / 1024 > _lastCounter / 1024)
		//	pollDevices();

		//printf("uhci: Processing transfers.\n");
		auto it = asyncSchedule.begin();
		while(it != asyncSchedule.end()) {
			it->progress();
			++it;
		}

		_lastFrame = frame;
		_lastCounter = counter;
	}
}))

COFIBER_ROUTINE(cofiber::no_future, bindDevice(mbus::Entity device), ([=] {
	using M = helix::AwaitMechanism;

	auto lane = helix::UniqueLane(COFIBER_AWAIT device.bind());

	// receive the device descriptor.
	helix::RecvInline<M> recv_resp;
	helix::PullDescriptor<M> pull_bar;
	helix::PullDescriptor<M> pull_irq;

	helix::submitAsync(lane, {
		helix::action(&recv_resp, kHelItemChain),
		helix::action(&pull_bar, kHelItemChain),
		helix::action(&pull_irq),
	}, helix::Dispatcher::global());

	COFIBER_AWAIT recv_resp.future();
	COFIBER_AWAIT pull_bar.future();
	COFIBER_AWAIT pull_irq.future();
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_bar.error());
	HEL_CHECK(pull_irq.error());

	managarm::hw::PciDevice resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());

	// run the UHCI driver.
	assert(resp.bars(0).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(1).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(2).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(3).io_type() == managarm::hw::IoType::NONE);
	assert(resp.bars(4).io_type() == managarm::hw::IoType::PORT);
	assert(resp.bars(5).io_type() == managarm::hw::IoType::NONE);
	HEL_CHECK(helEnableIo(pull_bar.descriptor().getHandle()));

	auto controller = std::make_shared<Controller>(resp.bars(4).address(),
			helix::UniqueIrq(pull_irq.descriptor()));
	controller->initialize();

	globalControllers.push_back(std::move(controller));
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

