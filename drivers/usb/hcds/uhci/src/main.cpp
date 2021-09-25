
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>
#include <deque>
#include <optional>
#include <functional>
#include <iostream>
#include <memory>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/io_space.hpp>
#include <arch/dma_pool.hpp>
#include <async/result.hpp>
#include <boost/intrusive/list.hpp>
#include <fafnir/dsl.hpp>
#include <helix/ipc.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/kernlet/compiler.hpp>
#include <protocols/mbus/client.hpp>
#include <protocols/usb/usb.hpp>
#include <protocols/usb/api.hpp>
#include <protocols/usb/server.hpp>

#include "uhci.hpp"
#include "spec.hpp"
#include "schedule.hpp"

std::vector<std::shared_ptr<Controller>> globalControllers;

// ----------------------------------------------------------------------------
// Memory management.
// ----------------------------------------------------------------------------

namespace {
	arch::contiguous_pool schedulePool;
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

arch::dma_pool *DeviceState::setupPool() {
	return &schedulePool;
}

arch::dma_pool *DeviceState::bufferPool() {
	return &schedulePool;
}

async::result<frg::expected<UsbError, std::string>> DeviceState::configurationDescriptor() {
	return _controller->configurationDescriptor(_device);
}

async::result<frg::expected<UsbError, Configuration>> DeviceState::useConfiguration(int number) {
	FRG_CO_TRY(co_await _controller->useConfiguration(_device, number));
	co_return Configuration{std::make_shared<ConfigurationState>(_controller,
			_device, number)};
}

async::result<frg::expected<UsbError>> DeviceState::transfer(ControlTransfer info) {
	return _controller->transfer(_device, 0, info);
}

// ----------------------------------------------------------------------------
// ConfigurationState
// ----------------------------------------------------------------------------

ConfigurationState::ConfigurationState(std::shared_ptr<Controller> controller,
		int device, int configuration)
: _controller{std::move(controller)}, _device(device), _configuration(configuration) {
	(void)_configuration;
}

async::result<frg::expected<UsbError, Interface>>
ConfigurationState::useInterface(int number, int alternative) {
	FRG_CO_TRY(co_await _controller->useInterface(_device, number, alternative));
	co_return Interface{std::make_shared<InterfaceState>(_controller, _device, number)};
}

// ----------------------------------------------------------------------------
// InterfaceState
// ----------------------------------------------------------------------------

InterfaceState::InterfaceState(std::shared_ptr<Controller> controller,
		int device, int interface)
: _controller{std::move(controller)}, _device(device), _interface(interface) {
	(void)_interface;
}

async::result<frg::expected<UsbError, Endpoint>>
InterfaceState::getEndpoint(PipeType type, int number) {
	co_return Endpoint{std::make_shared<EndpointState>(_controller,
			_device, type, number)};
}

// ----------------------------------------------------------------------------
// EndpointState
// ----------------------------------------------------------------------------

EndpointState::EndpointState(std::shared_ptr<Controller> controller,
		int device, PipeType type, int endpoint)
: _controller{std::move(controller)}, _device(device), _type(type), _endpoint(endpoint) { }

async::result<frg::expected<UsbError>> EndpointState::transfer(ControlTransfer info) {
	(void)info;
	assert(!"FIXME: Implement this");
	__builtin_unreachable();
}

async::result<frg::expected<UsbError, size_t>> EndpointState::transfer(InterruptTransfer info) {
	return _controller->transfer(_device, _type, _endpoint, info);
}

async::result<frg::expected<UsbError, size_t>> EndpointState::transfer(BulkTransfer info) {
	return _controller->transfer(_device, _type, _endpoint, info);
}

// ----------------------------------------------------------------------------
// Various stuff that needs to be moved to some USB core library.
// ----------------------------------------------------------------------------

void Enumerator::observeHub(std::shared_ptr<Hub> hub) {
	for(size_t port = 0; port < hub->numPorts(); port++)
		_observePort(hub, port);
}

async::detached Enumerator::_observePort(std::shared_ptr<Hub> hub, int port) {
	while(true)
		co_await _observationCycle(hub.get(), port);
}

async::result<void> Enumerator::_observationCycle(Hub *hub, int port) {
	std::unique_lock<async::mutex> enumerate_lock;

	// Wait until the device is connected.
	while(true) {
		auto s = co_await hub->pollState(port);

		if(s.status & hub_status::connect)
			break;
	}

	co_await _enumerateMutex.async_lock();
	enumerate_lock = std::unique_lock<async::mutex>{_enumerateMutex, std::adopt_lock};

	std::cout << "usb: Issuing reset on port " << port << std::endl;
	bool low_speed;
	if(!(co_await hub->issueReset(port, &low_speed)))
		co_return;
	if(low_speed)
		std::cout << "\e[31musb: Device is low speed!\e[39m" << std::endl;
	
	// Wait until the device is enabled.
	while(true) {
		auto s = co_await hub->pollState(port);

		// TODO: Handle disconnect here.
		if(s.status & hub_status::enable)
			break;
	}

//		std::cout << "usb: Enumerating device on port " << port << std::endl;
	co_await _controller->enumerateDevice(low_speed);
	enumerate_lock.unlock();
	
	// Wait until the device is disconnected.
	while(true) {
		auto s = co_await hub->pollState(port);

		if(!(s.status & hub_status::connect))
			break;
	}
}

namespace usb {
namespace standard_hub {

namespace {

namespace class_requests {
	static constexpr uint8_t getStatus = 0;
	static constexpr uint8_t clearFeature = 1;
	static constexpr uint8_t setFeature = 3;
	static constexpr uint8_t getDescriptor = 6;
}

namespace port_bits {
	static constexpr uint16_t connect = 0x01;
	static constexpr uint16_t enable = 0x02;
	static constexpr uint16_t reset = 0x10;
	static constexpr uint16_t lowSpeed = 0x200;
}

namespace port_features {
	//static constexpr uint16_t connect = 0;
	//static constexpr uint16_t enable = 1;
	static constexpr uint16_t reset = 4;
	static constexpr uint16_t connectChange = 16;
	static constexpr uint16_t enableChange = 17;
	static constexpr uint16_t resetChange = 20;
}

struct StandardHub final : Hub {
	StandardHub(Device device)
	: _device{std::move(device)}, _endpoint{nullptr} { }

	async::result<frg::expected<UsbError>> initialize();

private:
	async::detached _run();

public:
	size_t numPorts() override;
	async::result<PortState> pollState(int port) override;
	async::result<frg::expected<UsbError, bool>> issueReset(int port, bool *low_speed) override;

private:
	Device _device;
	Endpoint _endpoint;

	async::recurring_event _doorbell;
	std::vector<PortState> _state;
};

async::result<frg::expected<UsbError>> StandardHub::initialize() {
	// Read the generic USB device configuration.
	std::optional<int> cfg_number;
	std::optional<int> intf_number;
	std::optional<int> end_number;

	auto cfg_descriptor = FRG_CO_TRY(co_await _device.configurationDescriptor());
	walkConfiguration(cfg_descriptor, [&] (int type, size_t, void *, const auto &info) {
		if(type == descriptor_type::configuration) {
			assert(!cfg_number);
			cfg_number = info.configNumber.value();
		}else if(type == descriptor_type::interface) {
			assert(!intf_number);
			intf_number = info.interfaceNumber.value();
		}else if(type == descriptor_type::endpoint) {
			assert(!end_number);
			end_number = info.endpointNumber.value();
		}
	});

	auto cfg = FRG_CO_TRY(co_await _device.useConfiguration(cfg_number.value()));
	auto intf = FRG_CO_TRY(co_await cfg.useInterface(intf_number.value(), 0));
	_endpoint = FRG_CO_TRY(co_await intf.getEndpoint(PipeType::in, end_number.value()));

	// Read the hub class-specific descriptor.
	struct HubDescriptor : public DescriptorBase {
		uint8_t numPorts;
	};

	arch::dma_object<SetupPacket> get_descriptor{_device.setupPool()};
	get_descriptor->type = setup_type::targetDevice | setup_type::byClass
			| setup_type::toHost;
	get_descriptor->request = class_requests::getDescriptor;
	get_descriptor->value = 0x29 << 8;
	get_descriptor->index = intf_number.value();
	get_descriptor->length = sizeof(HubDescriptor);

	arch::dma_object<HubDescriptor> hub_descriptor{_device.bufferPool()};
	FRG_CO_TRY(co_await _device.transfer(ControlTransfer{kXferToHost,
			get_descriptor, hub_descriptor.view_buffer()}));

	_state.resize(hub_descriptor->numPorts, PortState{0, 0});
	_run();
	co_return {};
}

async::detached StandardHub::_run() {
	std::cout << "usb: Serving standard hub with "
			<< _state.size() << " ports." << std::endl;

	while(true) {
		arch::dma_array<uint8_t> report{_device.bufferPool(), (_state.size() + 1 + 7) / 8};
		(co_await _endpoint.transfer(InterruptTransfer{XferFlags::kXferToHost,
				report.view_buffer()})).unwrap();

//		std::cout << "usb: Hub report: " << (unsigned int)report[0] << std::endl;
		for(size_t port = 0; port < _state.size(); port++) {
			if(!(report[(port + 1) / 8] & (1 << ((port + 1) % 8))))
				continue;

			// Query issue a GetPortStatus request and inspect the status.
			arch::dma_object<SetupPacket> status_req{_device.setupPool()};
			status_req->type = setup_type::targetOther | setup_type::byClass
					| setup_type::toHost;
			status_req->request = class_requests::getStatus;
			status_req->value = 0;
			status_req->index = port + 1;
			status_req->length = 4;

			arch::dma_array<uint16_t> result{_device.bufferPool(), 2};
			(co_await _device.transfer(ControlTransfer{kXferToHost,
					status_req, result.view_buffer()})).unwrap();
//			std::cout << "usb: Port " << port << " status: "
//					<< result[0] << ", " << result[1] << std::endl;

			_state[port].status = 0;
			if(result[0] & port_bits::connect)
				_state[port].status |= hub_status::connect;
			if(result[0] & port_bits::enable)
				_state[port].status |= hub_status::enable;
			if(result[0] & port_bits::reset)
				_state[port].status |= hub_status::reset;

			// Inspect the status change bits and reset them.
			if(result[1] & port_bits::connect) {
				_state[port].changes |= hub_status::connect;
				_doorbell.raise();

				arch::dma_object<SetupPacket> clear_req{_device.setupPool()};
				clear_req->type = setup_type::targetOther | setup_type::byClass
						| setup_type::toDevice;
				clear_req->request = class_requests::clearFeature;
				clear_req->value = port_features::connectChange;
				clear_req->index = port + 1;
				clear_req->length = 0;

				(co_await _device.transfer(ControlTransfer{kXferToDevice,
						clear_req, arch::dma_buffer_view{}})).unwrap();
			}

			if(result[1] & port_bits::enable) {
				_state[port].changes |= hub_status::enable;
				_doorbell.raise();

				arch::dma_object<SetupPacket> clear_req{_device.setupPool()};
				clear_req->type = setup_type::targetOther | setup_type::byClass
						| setup_type::toDevice;
				clear_req->request = class_requests::clearFeature;
				clear_req->value = port_features::enableChange;
				clear_req->index = port + 1;
				clear_req->length = 0;

				(co_await _device.transfer(ControlTransfer{kXferToDevice,
						clear_req, arch::dma_buffer_view{}})).unwrap();
			}

			if(result[1] & port_bits::reset) {
				_state[port].changes |= hub_status::reset;
				_doorbell.raise();

				arch::dma_object<SetupPacket> clear_req{_device.setupPool()};
				clear_req->type = setup_type::targetOther | setup_type::byClass
						| setup_type::toDevice;
				clear_req->request = class_requests::clearFeature;
				clear_req->value = port_features::resetChange;
				clear_req->index = port + 1;
				clear_req->length = 0;

				(co_await _device.transfer(ControlTransfer{kXferToDevice,
						clear_req, arch::dma_buffer_view{}})).unwrap();
			}
		}
	}
}

size_t StandardHub::numPorts() {
	return _state.size();
}

async::result<PortState> StandardHub::pollState(int port) {
	while(true) {
		auto state = _state[port];
		if(state.changes) {
			_state[port].changes = 0;
			co_return state;
		}

		co_await _doorbell.async_wait();
	}
}

async::result<frg::expected<UsbError, bool>> StandardHub::issueReset(int port, bool *low_speed) {
	// Issue a SetPortFeature request to reset the port.
	arch::dma_object<SetupPacket> reset_req{_device.setupPool()};
	reset_req->type = setup_type::targetOther | setup_type::byClass
			| setup_type::toDevice;
	reset_req->request = class_requests::setFeature;
	reset_req->value = port_features::reset;
	reset_req->index = port + 1;
	reset_req->length = 0;

	FRG_CO_TRY(co_await _device.transfer(ControlTransfer{kXferToDevice,
			reset_req, arch::dma_buffer_view{}}));
	
	// Issue a GetPortStatus request to determine if the device is low-speed.
	arch::dma_object<SetupPacket> status_req{_device.setupPool()};
	status_req->type = setup_type::targetOther | setup_type::byClass
			| setup_type::toHost;
	status_req->request = class_requests::getStatus;
	status_req->value = 0;
	status_req->index = port + 1;
	status_req->length = 4;

	arch::dma_array<uint16_t> result{_device.bufferPool(), 2};
	FRG_CO_TRY(co_await _device.transfer(ControlTransfer{kXferToHost,
			status_req, result.view_buffer()}));
	*low_speed = (result[0] & port_bits::lowSpeed);

	co_return true;
}

} // anonymous namespace

std::shared_ptr<StandardHub> create(Device device) {
	return std::make_shared<StandardHub>(std::move(device));
}

} } // namespace usb::standard_hub

// ----------------------------------------------------------------------------
// Controller.
// ----------------------------------------------------------------------------

Controller::Controller(protocols::hw::Device hw_device, uintptr_t base,
		arch::io_space space, helix::UniqueIrq irq)
: _hwDevice{std::move(hw_device)}, _ioBase{base}, _ioSpace{space}, _irq{std::move(irq)},
		_lastFrame{0}, _frameCounter{0},
		_enumerator{this} {
	for(int i = 1; i < 128; i++) {
		_addressStack.push(i);
	}
}

void Controller::initialize() {
	// Host controller reset.
	_ioSpace.store(op_regs::command, command::hostReset(true));
	while((_ioSpace.load(op_regs::command) & command::hostReset) != 0) { }

	// TODO: What is the rationale of this check?
	auto initial_status = _ioSpace.load(op_regs::status);
	assert(!(initial_status & status::transactionIrq));
	assert(!(initial_status & status::errorIrq));

	// Setup the frame list.
	HelHandle list_handle;
	HEL_CHECK(helAllocateMemory(4096, 0, nullptr, &list_handle));
	void *list_mapping;
	HEL_CHECK(helMapMemory(list_handle, kHelNullHandle,
			nullptr, 0, 4096, kHelMapProtRead | kHelMapProtWrite, &list_mapping));
	
	_frameList = (FrameList *)list_mapping;
	for(int i = 0; i < 1024; i++)
		_frameList->entries[i].store(FrameListPointer{}._bits);

	// Pass the frame list to the controller and run it.
	uintptr_t list_physical;
	HEL_CHECK(helPointerPhysical(_frameList, &list_physical));
	assert((list_physical % 0x1000) == 0);
	_ioSpace.store(op_regs::frameListBase, list_physical);
	_ioSpace.store(op_regs::command, command::runStop(true));
	
	// Enable interrupts.
	_ioSpace.store(op_regs::irqEnable, irq::timeout(true) | irq::resume(true)
			| irq::transaction(true) | irq::shortPacket(true));

	_enumerator.observeHub(std::make_shared<RootHub>(this));
	_handleIrqs();
	_refreshFrame();
}

async::detached Controller::_handleIrqs() {
	co_await connectKernletCompiler();

	std::vector<uint8_t> kernlet_program;
	fnr::emit_to(std::back_inserter(kernlet_program),
		// Load the USBSTS register.
		fnr::scope_push{} (
			fnr::intrin{"__pio_read16", 1, 1} (
				fnr::binding{0} // UHCI PIO offset (bound to slot 0).
					 + fnr::literal{op_regs::status.offset()}
			) & fnr::literal{static_cast<uint16_t>(status::transactionIrq(true)
					| status::errorIrq(true)
					| status::hostProcessError(true)
					| status::hostSystemError(true))}
		),
		// Ack the IRQ iff one of the bits was set.
		fnr::check_if{},
			fnr::scope_get{0},
		fnr::then{},
			// Write back the interrupt bits to USBSTS to deassert the IRQ.
			fnr::intrin{"__pio_write16", 2, 0} (
				fnr::binding{0} // UHCI PIO offset (bound to slot 0).
					+ fnr::literal{op_regs::status.offset()},
				fnr::scope_get{0}
			),
			// Trigger the bitset event (bound to slot 1).
			fnr::intrin{"__trigger_bitset", 2, 0} (
				fnr::binding{1},
				fnr::scope_get{0}
			),
			fnr::scope_push{} ( fnr::literal{1} ),
		fnr::else_then{},
			fnr::scope_push{} ( fnr::literal{2} ),
		fnr::end{}
	);

	auto kernlet_object = co_await compile(kernlet_program.data(),
			kernlet_program.size(), {BindType::offset, BindType::bitsetEvent});

	HelHandle event_handle;
	HEL_CHECK(helCreateBitsetEvent(&event_handle));
	helix::UniqueDescriptor event{event_handle};

	HelKernletData data[2];
	data[0].handle = _ioBase;
	data[1].handle = event.getHandle();
	HelHandle bound_handle;
	HEL_CHECK(helBindKernlet(kernlet_object.getHandle(), data, 2, &bound_handle));
	HEL_CHECK(helAutomateIrq(_irq.getHandle(), 0, bound_handle));

	co_await _hwDevice.enableBusIrq();

	// Clear the IRQ in case it was pending while we attached the kernlet.
	HEL_CHECK(helAcknowledgeIrq(_irq.getHandle(), kHelAckKick | kHelAckClear, 0));

	uint64_t sequence = 0;
	while(true) {
		auto await = co_await helix_ng::awaitEvent(event, sequence);
		HEL_CHECK(await.error());
		sequence = await.sequence();

		auto bits = arch::bit_value<uint16_t>(await.bitset());

		assert(!(bits & status::hostProcessError));
		assert(!(bits & status::hostSystemError));

		if(bits & status::errorIrq)
			printf("\e[31muhci: Error interrupt\e[39m\n");
		if((bits & status::transactionIrq) || (bits & status::errorIrq)) {
			//printf("uhci: Processing transfers.\n");
			_progressSchedule();
		}
	}
}

async::detached Controller::_refreshFrame() {
	while(true) {
//		std::cout << "uhci: Frame update" << std::endl;
		_updateFrame();

		uint64_t tick;
		HEL_CHECK(helGetClock(&tick));

		helix::AwaitClock await_clock;
		auto &&submit = helix::submitAwaitClock(&await_clock, tick + 500'000'000,
				helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(await_clock.error());
	}
}

void Controller::_updateFrame() {
	auto frame = _ioSpace.load(op_regs::frameNumber);
	auto counter = (frame >= _lastFrame) ? (_frameCounter + frame - _lastFrame)
			: (_frameCounter + 2048 - _lastFrame + frame);

	if(counter / 1024 > _frameCounter / 1024) {
		for(int port = 0; port < 2; port++) {
			auto port_space = _ioSpace.subspace(0x10 + (2 * port));
			auto sc = port_space.load(port_regs::statusCtrl);
//			std::cout << "uhci: Port " << port << " status/control: "
//					<< static_cast<uint16_t>(sc) << std::endl;

			// Extract the status bits.
			_portState[port].status = 0;
			if(sc & port_status_ctrl::connectStatus)
				_portState[port].status |= hub_status::connect;
			if(sc & port_status_ctrl::enableStatus)
				_portState[port].status |= hub_status::enable;

			// Extract the change bits.
			if(sc & port_status_ctrl::connectChange) {
				_portState[port].changes |= hub_status::connect;
				_portDoorbell.raise();
			}
			if(sc & port_status_ctrl::enableChange) {
				_portState[port].changes |= hub_status::enable;
				_portDoorbell.raise();
			}

			// Write-back clears the change bits.
			port_space.store(port_regs::statusCtrl, sc);
		}
	}

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

size_t Controller::RootHub::numPorts() {
	return 2;
}

async::result<PortState> Controller::RootHub::pollState(int port) {
	while(true) {
		auto state = _controller->_portState[port];
		if(state.changes) {
			_controller->_portState[port].changes = 0;
			co_return state;
		}

		co_await _controller->_portDoorbell.async_wait();
	}
}

async::result<frg::expected<UsbError, bool>>
Controller::RootHub::issueReset(int port, bool *low_speed) {
	auto port_space = _controller->_ioSpace.subspace(0x10 + (2 * port));

	// Reset the port for 50 ms.
	port_space.store(port_regs::statusCtrl, port_status_ctrl::portReset(true));

	uint64_t tick;
	HEL_CHECK(helGetClock(&tick));

	helix::AwaitClock await_clock;
	auto &&submit = helix::submitAwaitClock(&await_clock, tick + 50'000'000,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(await_clock.error());
	
	// Disable the reset line.
	port_space.store(port_regs::statusCtrl, port_status_ctrl::portReset(false));

	// Linux issues a 10us wait here, probably to wait until reset is turned off in hardware.
	usleep(10);

	// Enable the port and wait until it is available.
	port_space.store(port_regs::statusCtrl, port_status_ctrl::enableStatus(true));

	uint64_t start;
	HEL_CHECK(helGetClock(&start));
	while(true) {
		auto sc = port_space.load(port_regs::statusCtrl);
		if((sc & port_status_ctrl::enableStatus))
			break;
	
		uint64_t now;
		HEL_CHECK(helGetClock(&now));
		if(now - start > 1000'000'000) {
			std::cout << "\e[31muhci: Could not enable device after reset\e[39m" << std::endl;
			co_return false;
		}
	}
	
	auto sc = port_space.load(port_regs::statusCtrl);
	*low_speed = (sc & port_status_ctrl::lowSpeed);

	// Similar to USB standard hubs we do not reset the enable-change bit.
	_controller->_portState[port].status |= hub_status::enable;
	_controller->_portState[port].changes |= hub_status::reset;
	_controller->_portDoorbell.raise();

	co_return true;
}

async::result<void> Controller::enumerateDevice(bool low_speed) {
	// This queue will become the default control pipe of our new device.
	auto queue = new QueueEntity{arch::dma_object<QueueHead>{&schedulePool}};
	_linkAsync(queue);

	// Allocate an address for the device.
	assert(!_addressStack.empty());
	auto address = _addressStack.front();
	_addressStack.pop();

	arch::dma_object<SetupPacket> set_address{&schedulePool};
	set_address->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toDevice;
	set_address->request = request_type::setAddress;
	set_address->value = address;
	set_address->index = 0;
	set_address->length = 0;

	(co_await _directTransfer(0, 0, ControlTransfer{kXferToDevice,
			set_address, arch::dma_buffer_view{}}, queue, low_speed, 8)).unwrap();

	// Enquire the maximum packet size of the default control pipe.
	arch::dma_object<SetupPacket> get_header{&schedulePool};
	get_header->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toHost;
	get_header->request = request_type::getDescriptor;
	get_header->value = descriptor_type::device << 8;
	get_header->index = 0;
	get_header->length = 8;

	arch::dma_object<DeviceDescriptor> descriptor{&schedulePool};
	(co_await _directTransfer(address, 0, ControlTransfer{kXferToHost,
			get_header, descriptor.view_buffer().subview(0, 8)}, queue, low_speed, 8)).unwrap();

	_activeDevices[address].lowSpeed = low_speed;
	_activeDevices[address].controlStates[0].queueEntity = queue;
	_activeDevices[address].controlStates[0].maxPacketSize = descriptor->maxPacketSize;

	// Read the rest of the device descriptor.
	arch::dma_object<SetupPacket> get_descriptor{&schedulePool};
	get_descriptor->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toHost;
	get_descriptor->request = request_type::getDescriptor;
	get_descriptor->value = descriptor_type::device << 8;
	get_descriptor->index = 0;
	get_descriptor->length = sizeof(DeviceDescriptor);

	(co_await transfer(address, 0, ControlTransfer{kXferToHost,
			get_descriptor, descriptor.view_buffer()})).unwrap();
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

	std::cout << "uhci: Enumerating device of class: 0x" << class_code
			<< ", sub class: 0x" << sub_class << ", protocol: 0x" << protocol << std::endl;

	if(descriptor->deviceClass == 0x09 && descriptor->deviceSubclass == 0
			&& descriptor->deviceProtocol == 0) {
		auto state = std::make_shared<DeviceState>(shared_from_this(), address);
		auto hub = usb::standard_hub::create(Device{std::move(state)});
		(co_await hub->initialize()).unwrap();
		_enumerator.observeHub(std::move(hub));
	}

	mbus::Properties mbus_desc {
		{ "usb.type", mbus::StringItem{"device"}},
		{ "usb.vendor", mbus::StringItem{vendor}},
		{ "usb.product", mbus::StringItem{product}},
		{ "usb.class", mbus::StringItem{class_code}},
		{ "usb.subclass", mbus::StringItem{sub_class}},
		{ "usb.protocol", mbus::StringItem{protocol}},
		{ "usb.release", mbus::StringItem{release}}
	};
	
	auto root = co_await mbus::Instance::global().getRoot();
	
	char name[3];
	sprintf(name, "%.2x", address);

	auto handler = mbus::ObjectHandler{}
	.withBind([=] () -> async::result<helix::UniqueDescriptor> {
		helix::UniqueLane local_lane, remote_lane;
		std::tie(local_lane, remote_lane) = helix::createStream();
		auto state = std::make_shared<DeviceState>(shared_from_this(), address);
		protocols::usb::serve(Device{std::move(state)}, std::move(local_lane));

		co_return std::move(remote_lane);
	});

	co_await root.createObject(name, mbus_desc, std::move(handler));
}

// ------------------------------------------------------------------------
// Controller: Device management.
// ------------------------------------------------------------------------

async::result<frg::expected<UsbError, std::string>>
Controller::configurationDescriptor(int address) {
	// Read the descriptor header that contains the hierachy size.
	arch::dma_object<SetupPacket> get_header{&schedulePool};
	get_header->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toHost;
	get_header->request = request_type::getDescriptor;
	get_header->value = descriptor_type::configuration << 8;
	get_header->index = 0;
	get_header->length = sizeof(ConfigDescriptor);

	arch::dma_object<ConfigDescriptor> header{&schedulePool};
	FRG_CO_TRY(co_await transfer(address, 0, ControlTransfer{kXferToHost,
			get_header, header.view_buffer()}));
	assert(header->length == sizeof(ConfigDescriptor));

	// Read the whole descriptor hierachy.
	arch::dma_object<SetupPacket> get_descriptor{&schedulePool};
	get_descriptor->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toHost;
	get_descriptor->request = request_type::getDescriptor;
	get_descriptor->value = descriptor_type::configuration << 8;
	get_descriptor->index = 0;
	get_descriptor->length = header->totalLength;

	arch::dma_buffer descriptor{&schedulePool, header->totalLength};
	FRG_CO_TRY(co_await transfer(address, 0, ControlTransfer{kXferToHost,
			get_descriptor, descriptor}));

	// TODO: This function should return a arch::dma_buffer!
	std::string copy((char *)descriptor.data(), header->totalLength);
	co_return std::move(copy);
}

async::result<frg::expected<UsbError>>
Controller::useConfiguration(int address, int configuration) {
	arch::dma_object<SetupPacket> set_config{&schedulePool};
	set_config->type = setup_type::targetDevice | setup_type::byStandard
			| setup_type::toDevice;
	set_config->request = request_type::setConfig;
	set_config->value = configuration;
	set_config->index = 0;
	set_config->length = 0;

	FRG_CO_TRY(co_await transfer(address, 0, ControlTransfer{kXferToDevice,
			set_config, arch::dma_buffer_view{}}));
	co_return{};
}

async::result<frg::expected<UsbError>>
Controller::useInterface(int address, int interface, int alternative) {
	auto descriptor = FRG_CO_TRY(co_await configurationDescriptor(address));
	walkConfiguration(descriptor, [&] (int type, size_t length, void *p, const auto &info) {
		(void)length;

		if(type != descriptor_type::endpoint)
			return;
		auto desc = (EndpointDescriptor *)p;

		// TODO: Pay attention to interface/alternative.
		std::cout << "uhci: Interval is " << (int)desc->interval << std::endl;
		
		int pipe = info.endpointNumber.value();
		QueueEntity *entity;
		if(info.endpointIn.value()) {
			std::cout << "uhci: Setting up IN endpoint " << pipe << std::endl;
			entity = new QueueEntity{arch::dma_object<QueueHead>{&schedulePool}};
			_activeDevices[address].inStates[pipe].maxPacketSize = desc->maxPacketSize;
			_activeDevices[address].inStates[pipe].queueEntity = entity;
		}else{
			std::cout << "uhci: Setting up OUT endpoint " << pipe << std::endl;
			entity = new QueueEntity{arch::dma_object<QueueHead>{&schedulePool}};
			_activeDevices[address].outStates[pipe].maxPacketSize = desc->maxPacketSize;
			_activeDevices[address].outStates[pipe].queueEntity = entity;
		}

		auto order = 1 << (CHAR_BIT * sizeof(int) - __builtin_clz(desc->interval) - 1);
		std::cout << "uhci: Using order " << order << std::endl;
		this->_linkInterrupt(entity, order, 0);
//TODO: For bulk: this->_linkAsync(entity);

	});
	co_return {};
}

// ------------------------------------------------------------------------
// Controller: Transfer functions.
// ------------------------------------------------------------------------

async::result<frg::expected<UsbError>>
Controller::transfer(int address, int pipe, ControlTransfer info) {
	auto device = &_activeDevices[address];
	auto endpoint = &device->controlStates[pipe];
	
	auto transaction = _buildControl(address, pipe, info.flags,
			info.setup, info.buffer, device->lowSpeed, endpoint->maxPacketSize);
	auto future = transaction->voidPromise.get_future();
	_linkTransaction(endpoint->queueEntity, transaction);
	co_return *(co_await future.get());
}

async::result<frg::expected<UsbError, size_t>>
Controller::transfer(int address, PipeType type, int pipe,
		InterruptTransfer info) {
	// TODO: Ensure pipe type matches transfer direction.
	auto device = &_activeDevices[address];
	EndpointSlot *endpoint;
	if(type == PipeType::in) {
		endpoint = &device->inStates[pipe];
	}else{
		assert(type == PipeType::out);
		assert(!info.allowShortPackets);
		endpoint = &device->outStates[pipe];
	}

	auto transaction = _buildInterruptOrBulk(address, pipe, info.flags,
			info.buffer, device->lowSpeed, endpoint->maxPacketSize, info.allowShortPackets);
	auto future = transaction->promise.get_future();
	_linkTransaction(endpoint->queueEntity, transaction);
	co_return *(co_await future.get());
}

async::result<frg::expected<UsbError, size_t>>
Controller::transfer(int address, PipeType type, int pipe,
		BulkTransfer info) {
	// TODO: Ensure pipe type matches transfer direction.
	auto device = &_activeDevices[address];
	EndpointSlot *endpoint;
	if(type == PipeType::in) {
		assert(info.flags == kXferToHost);
		endpoint = &device->inStates[pipe];
	}else{
		assert(type == PipeType::out);
		assert(info.flags == kXferToDevice);
		assert(!info.allowShortPackets);
		endpoint = &device->outStates[pipe];
	}

	auto transaction = _buildInterruptOrBulk(address, pipe, info.flags,
			info.buffer, device->lowSpeed, endpoint->maxPacketSize, info.allowShortPackets);
	auto future = transaction->promise.get_future();
	_linkTransaction(endpoint->queueEntity, transaction);
	co_return *(co_await future.get());
}

auto Controller::_buildControl(int address, int pipe, XferFlags dir,
		arch::dma_object_view<SetupPacket> setup, arch::dma_buffer_view buffer,
		bool low_speed, size_t max_packet_size) -> Transaction * {
	assert((dir == kXferToDevice) || (dir == kXferToHost));

	size_t num_data = (buffer.size() + max_packet_size - 1) / max_packet_size;
	arch::dma_array<TransferDescriptor> transfers{&schedulePool, num_data + 2};

	transfers[0].status.store(td_status::active(true) | td_status::detectShort(true)
			| td_status::lowSpeed(low_speed));
	transfers[0].token.store(td_token::pid(Packet::setup) | td_token::address(address)
			| td_token::pipe(pipe) | td_token::length(sizeof(SetupPacket) - 1));
	transfers[0]._bufferPointer = TransferBufferPointer::from(setup.data());
	transfers[0]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[1]);

	size_t progress = 0;
	for(size_t i = 0; i < num_data; i++) {
		size_t chunk = std::min(max_packet_size, buffer.size() - progress);
		assert(chunk);
		transfers[i + 1].status.store(td_status::active(true) | td_status::detectShort(true)
				| td_status::lowSpeed(low_speed));
		transfers[i + 1].token.store(td_token::pid(dir == kXferToDevice ? Packet::out : Packet::in)
				| td_token::toggle(i % 2 == 0) | td_token::address(address)
				| td_token::pipe(pipe) | td_token::length(chunk - 1));
		transfers[i + 1]._bufferPointer
				= TransferBufferPointer::from((char *)buffer.data() + progress);
		transfers[i + 1]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[i + 2]);
		progress += chunk;
	}

	transfers[num_data + 1].status.store(td_status::active(true) | td_status::completionIrq(true)
			| td_status::lowSpeed(low_speed));
	transfers[num_data + 1].token.store(td_token::pid(dir == kXferToDevice ? Packet::in : Packet::out)
			| td_token::toggle(true) | td_token::address(address)
			| td_token::pipe(pipe) | td_token::length(0x7FF));

	return new Transaction{std::move(transfers)};
}

auto Controller::_buildInterruptOrBulk(int address, int pipe, XferFlags dir,
		arch::dma_buffer_view buffer,
		bool low_speed, size_t max_packet_size,
		bool allow_short_packet) -> Transaction * {
	assert((dir == kXferToDevice) || (dir == kXferToHost));
//	std::cout << "_buildInterruptOrBulk. Address: " << address
//			<< ", pipe: " << pipe << ", direction: " << dir
//			<< ", maxPacketSize: " << max_packet_size
//			<< ", buffer size: " << buffer.size() << std::endl;

	size_t num_data = (buffer.size() + max_packet_size - 1) / max_packet_size;
	arch::dma_array<TransferDescriptor> transfers{&schedulePool, num_data};

	size_t progress = 0;
	for(size_t i = 0; i < num_data; i++) {
		size_t chunk = std::min(max_packet_size, buffer.size() - progress);
		assert(chunk);
		// TODO: Only set detectShort bit if allow_short_packet is true?
		transfers[i].status.store(td_status::active(true)
				| td_status::completionIrq(i + 1 == num_data) | td_status::detectShort(true)
				| td_status::lowSpeed(low_speed));
		transfers[i].token.store(td_token::pid(dir == kXferToDevice ? Packet::out : Packet::in)
				| td_token::address(address) | td_token::pipe(pipe)
				| td_token::length(chunk - 1));
		transfers[i]._bufferPointer = TransferBufferPointer::from((char *)buffer.data() + progress);

		if(i + 1 < num_data)
			transfers[i]._linkPointer = TransferDescriptor::LinkPointer::from(&transfers[i + 1]);
		progress += chunk;
	}

	auto transaction = new Transaction{std::move(transfers), allow_short_packet};
	transaction->autoToggle = true;
	return transaction;
}

async::result<frg::expected<UsbError>>
Controller::_directTransfer(int address, int pipe, ControlTransfer info,
		QueueEntity *queue, bool low_speed, size_t max_packet_size) {
	auto transaction = _buildControl(address, pipe, info.flags,
			info.setup, info.buffer, low_speed, max_packet_size);
	auto future = transaction->voidPromise.get_future();
	_linkTransaction(queue, transaction);
	co_return *(co_await future.get());
}

// ----------------------------------------------------------------
// Controller: Schedule manipulation functions.
// ----------------------------------------------------------------

void Controller::_linkInterrupt(QueueEntity *entity, int order, int index) {
	assert(order > 0 && order <= 1024);
	assert(index < order);

	// Try to find a periodic entity with lower order that we link to.
	int so = order >> 1;
	while(so) {
		/*auto n = (so - 1) + (index & (so - 1));
		if(!_interruptSchedule[n].empty()) {
			std::cout << "Linking to a lower order. This is untested" << std::endl;
			auto successor = &_interruptSchedule[n].front();
			entity->head->_linkPointer = QueueHead::LinkPointer::from(successor->head.data());
			break;
		}*/
		so >>= 1;
	}

	// If there is no lower-order periodic entity, link to the async schedule.
	if(!so) {
		assert(!_asyncSchedule.empty());
		auto successor = &_asyncSchedule.front();
		entity->head->_linkPointer = QueueHead::LinkPointer::from(successor->head.data());
	}

	// Link to the back of this order/index of the periodic schedule.
	auto n = (order - 1) + index;
	if(_interruptSchedule[n].empty()) {
		// Link the front of the schedule to the new entity.
		if(order == 1024) {
			_frameList->entries[index].store(FrameListPointer::from(entity->head.data())._bits);
		}else{
			_linkIntoScheduleTree(order << 1, index, entity);
			_linkIntoScheduleTree(order << 1, index + order, entity);
		}
	}else{
		auto predecessor = &_interruptSchedule[n].back();
		predecessor->head->_linkPointer = QueueHead::LinkPointer::from(entity->head.data());
	}
	_interruptSchedule[n].push_back(*entity);
	_activeEntities.push_back(entity);
}

void Controller::_linkAsync(QueueEntity *entity) {
	// Link to the back of the asynchronous schedule.
	if(_asyncSchedule.empty()) {
		// Link the front of the schedule to the new entity.
		_linkIntoScheduleTree(1, 0, entity);
	}else{
		_asyncSchedule.back().head->_linkPointer
				= QueueHead::LinkPointer::from(entity->head.data());
	}
	_asyncSchedule.push_back(*entity);
	_activeEntities.push_back(entity);
}

void Controller::_linkIntoScheduleTree(int order, int index, QueueEntity *entity) {
	assert(order > 0 && order <= 1024);
	assert(index < order);

	auto n = (order - 1) + index;
	if(_interruptSchedule[n].empty()) {
		if(order == 1024) {
			_frameList->entries[index].store(FrameListPointer::from(entity->head.data())._bits);
		}else{
			_linkIntoScheduleTree(order << 1, index, entity);
			_linkIntoScheduleTree(order << 1, index + order, entity);
		}
	}else{
		auto predecessor = &_interruptSchedule[n].back();
		predecessor->head->_linkPointer = QueueHead::LinkPointer::from(entity->head.data());
	}
}

void Controller::_linkTransaction(QueueEntity *queue, Transaction *transaction) {
	if(queue->transactions.empty()) {
		// Update the toggle state of the transaction.
		if(transaction->autoToggle) {
			bool state = queue->toggleState;
			for(size_t i = 0; i < transaction->transfers.size(); i++) {
				transaction->transfers[i].token.store(transaction->transfers[i].token.load()
						| td_token::toggle(state));
				state = !state;
			}
		}

		queue->head->_elementPointer = QueueHead::LinkPointer::from(&transaction->transfers[0]);
	}

	queue->transactions.push_back(*transaction);
}

void Controller::_progressSchedule() {
	auto it = _activeEntities.begin();
	while(it != _activeEntities.end()) {
		_progressQueue(*it);
		++it;
	}
}

void Controller::_progressQueue(QueueEntity *entity) {
	if(entity->transactions.empty())
		return;

	auto front = &entity->transactions.front();

	auto handleError = [&] () {
		// TODO: This could also mean that the TD is not retired because of SPD.
		// TODO: Unify this case with the transaction success case above.
		std::cout << "\e[31muhci: Transfer error!\e[39m" << std::endl;
		_dump(front);
		
		// Clean up the Queue.
		entity->transactions.pop_front();
//TODO:		_reclaim(front);
	};
	
	auto decodeLength = [] (size_t n) -> size_t {
		if(n == 0x7FF)
			return 0;
		assert(n <= 0x4FF);
		return n + 1;
	};

	while(front->numComplete < front->transfers.size()) {
		auto &transfer = front->transfers[front->numComplete];
		auto status = transfer.status.load();
		if(status & td_status::active) {
			return;
		}else if(status & td_status::errorBits) {
			handleError();
			return;
		}
		assert(!(status & td_status::stalled));

		auto n = status & td_status::actualLength;
		front->numComplete++;
		front->lengthComplete += decodeLength(n);
		
		// We advance the toggleState on each successful transaction for
		// each pipe type, not only for bulk/interrupt. This does not really hurt.
		entity->toggleState = !entity->toggleState;
		
		// Short packets end the transfer without advancing the queue.
		if(n != (transfer.token.load() & td_token::length)) {
			if(!front->allowShortPackets) {
				std::cout << "uhci: Actual length is " << n
						<< ", while we expect " << (transfer.token.load() & td_token::length)
						<< ", auto toggle is " << front->autoToggle
						<< std::endl;
				throw std::runtime_error("uhci: Short packet not allowed");
			}
			break;
		}
	}

	//printf("Transfer complete!\n");
	front->promise.set_value(front->lengthComplete);
	front->voidPromise.set_value(UsbError{});

	// Schedule the next transaction.
	entity->transactions.pop_front();
	if(entity->transactions.empty()) {
		entity->head->_elementPointer = QueueHead::LinkPointer{};
	}else{
		auto front = &entity->transactions.front();
		entity->head->_elementPointer = QueueHead::LinkPointer::from(&front->transfers[0]);
	}

	// Reclaim memory.
	_reclaim(front);
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
	for(size_t i = 0; i < transaction->transfers.size(); i++) {
		std::cout << "    TD " << i << ":";
		transaction->transfers[i].dumpStatus();
		std::cout << std::endl;
	}
}

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

async::detached bindController(mbus::Entity entity) {
	protocols::hw::Device device(co_await entity.bind());
	auto info = co_await device.getPciInfo();
	assert(info.barInfo[4].ioType == protocols::hw::IoType::kIoTypePort);
	auto bar = co_await device.accessBar(4);
	auto irq = co_await device.accessIrq();

	// TODO: Disable the legacy support registers of all UHCI devices
	// before using one of them!
	auto legsup = co_await device.loadPciSpace(kPciLegacySupport, 2);
	std::cout << "uhci: Legacy support register: " << legsup << std::endl;

	HEL_CHECK(helEnableIo(bar.getHandle()));
	
	arch::io_space base = arch::global_io.subspace(info.barInfo[4].address);
	auto controller = std::make_shared<Controller>(std::move(device),
			info.barInfo[4].address, base, std::move(irq));
	controller->initialize();

	globalControllers.push_back(std::move(controller));
}

async::detached observeControllers() {
	auto root = co_await mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-class", "0c"),
		mbus::EqualsFilter("pci-subclass", "03"),
		mbus::EqualsFilter("pci-interface", "00")
	});

	auto handler = mbus::ObserverHandler{}
	.withAttach([] (mbus::Entity entity, mbus::Properties) {
		std::cout << "uhci: Detected controller" << std::endl;
		bindController(std::move(entity));
	});

	co_await root.linkObserver(std::move(filter), std::move(handler));
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("uhci: Starting driver\n");

//	HEL_CHECK(helSetPriority(kHelThisThread, 2));
	
	{
		async::queue_scope scope{helix::globalQueue()};
		observeControllers();
	}

	async::run_forever(helix::globalQueue()->run_token(), helix::currentDispatcher);
	
	return 0;
}

