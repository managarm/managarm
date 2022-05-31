#include <protocols/usb/hub.hpp>

#include <async/recurring-event.hpp>

// ----------------------------------------------------------------
// Enumerator.
// ----------------------------------------------------------------

void Enumerator::observeHub(std::shared_ptr<Hub> hub) {
	for (size_t port = 0; port < hub->numPorts(); port++)
		observePort_(hub, port);
}

async::detached Enumerator::observePort_(std::shared_ptr<Hub> hub, int port) {
	while (true)
		co_await observationCycle_(hub, port);
}

async::result<void> Enumerator::observationCycle_(std::shared_ptr<Hub> hub, int port) {
	std::unique_lock<async::mutex> enumerate_lock;

	// Wait until the device is connected.
	while (true) {
		auto s = co_await hub->pollState(port);

		if (s.status & HubStatus::connect)
			break;
	}

	// TODO(qookie): enumerateMutex_ should be moved into the controller code, as we should be able to submit multiple enumerations at once on XHCI for example
	co_await enumerateMutex_.async_lock();
	enumerate_lock = std::unique_lock<async::mutex>{enumerateMutex_, std::adopt_lock};

	std::cout << "usb: Issuing reset on port " << port << std::endl;

	DeviceSpeed speed; 

	if (auto v = co_await hub->issueReset(port); !v)
		co_return;
	else
		speed = v.value();

	std::cout << "usb: Waiting for device to become enabled on port " << port << std::endl;

	// Wait until the device is enabled.
	while (true) {
		auto s = co_await hub->pollState(port);

		// TODO: Handle disconnect here.
		if (s.status & HubStatus::enable)
			break;
	}

	std::cout << "usb: Enumerating device on port " << port << std::endl;
	co_await controller_->enumerateDevice(hub, port, speed);
	enumerate_lock.unlock();

	// Wait until the device is disconnected.
	while(true) {
		auto s = co_await hub->pollState(port);

		if(!(s.status & HubStatus::connect))
			break;
	}
}

// ----------------------------------------------------------------
// StandardHub.
// ----------------------------------------------------------------

namespace {

namespace ClassRequests {
	static constexpr uint8_t getStatus = 0;
	static constexpr uint8_t clearFeature = 1;
	static constexpr uint8_t setFeature = 3;
	static constexpr uint8_t getDescriptor = 6;
}

namespace PortBits {
	static constexpr uint16_t connect = 0x01;
	static constexpr uint16_t enable = 0x02;
	static constexpr uint16_t reset = 0x10;
	static constexpr uint16_t lowSpeed = 0x200;
	static constexpr uint16_t highSpeed = 0x400;
}

namespace PortFeatures {
	//static constexpr uint16_t connect = 0;
	//static constexpr uint16_t enable = 1;
	static constexpr uint16_t reset = 4;
	static constexpr uint16_t connectChange = 16;
	static constexpr uint16_t enableChange = 17;
	static constexpr uint16_t resetChange = 20;
}

struct StandardHub final : Hub {
	StandardHub(std::shared_ptr<Hub> parentHub, Device device, size_t port)
	: Hub{parentHub, port}, device_{std::move(device)}, endpoint_{nullptr} { }

	async::result<frg::expected<UsbError>> initialize();

private:
	async::detached run_();

	HubCharacteristics characteristics_;

public:
	size_t numPorts() override;
	async::result<PortState> pollState(int port) override;
	async::result<frg::expected<UsbError, DeviceSpeed>> issueReset(int port) override;

	frg::expected<UsbError, HubCharacteristics> getCharacteristics() override {
		return characteristics_;
	}

private:
	Device device_;
	Endpoint endpoint_;

	async::recurring_event doorbell_;
	std::vector<PortState> state_;
};

async::result<frg::expected<UsbError>> StandardHub::initialize() {
	// Read the generic USB device configuration.
	std::optional<int> cfgNumber;
	std::optional<int> intfNumber;
	std::optional<int> endNumber;

	auto cfgDescriptor = FRG_CO_TRY(co_await device_.configurationDescriptor());
	walkConfiguration(cfgDescriptor, [&] (int type, size_t, void *, const auto &info) {
		if(type == descriptor_type::configuration) {
			assert(!cfgNumber);
			cfgNumber = info.configNumber.value();
		}else if(type == descriptor_type::interface) {
			assert(!intfNumber);
			intfNumber = info.interfaceNumber.value();
		}else if(type == descriptor_type::endpoint) {
			assert(!endNumber);
			endNumber = info.endpointNumber.value();
		}
	});

	auto cfg = FRG_CO_TRY(co_await device_.useConfiguration(cfgNumber.value()));
	auto intf = FRG_CO_TRY(co_await cfg.useInterface(intfNumber.value(), 0));
	endpoint_ = FRG_CO_TRY(co_await intf.getEndpoint(PipeType::in, endNumber.value()));

	// Read the hub class-specific descriptor.
	struct [[gnu::packed]] HubDescriptor : public DescriptorBase {
		uint8_t numPorts;
		uint16_t hubCharacteristics;
	};

	arch::dma_object<SetupPacket> getDescriptor{device_.setupPool()};
	getDescriptor->type = setup_type::targetDevice | setup_type::byClass
			| setup_type::toHost;
	getDescriptor->request = ClassRequests::getDescriptor;
	getDescriptor->value = 0x29 << 8;
	getDescriptor->index = intfNumber.value();
	getDescriptor->length = sizeof(HubDescriptor);

	arch::dma_object<HubDescriptor> hubDescriptor{device_.bufferPool()};
	FRG_CO_TRY(co_await device_.transfer(ControlTransfer{kXferToHost,
			getDescriptor, hubDescriptor.view_buffer()}));

	state_.resize(hubDescriptor->numPorts, PortState{0, 0});

	auto rawThinkTime = (hubDescriptor->hubCharacteristics >> 5) & 0b11;
	characteristics_.ttThinkTime = 8 * (1 + rawThinkTime);

	run_();
	co_return {};
}

async::detached StandardHub::run_() {
	std::cout << "usb: Serving standard hub with "
			<< state_.size() << " ports." << std::endl;

	while(true) {
		arch::dma_array<uint8_t> report{device_.bufferPool(), (state_.size() + 1 + 7) / 8};
		(co_await endpoint_.transfer(InterruptTransfer{XferFlags::kXferToHost,
				report.view_buffer()})).unwrap();

//		std::cout << "usb: Hub report: " << (unsigned int)report[0] << std::endl;
		for(size_t port = 0; port < state_.size(); port++) {
			if(!(report[(port + 1) / 8] & (1 << ((port + 1) % 8))))
				continue;

			// Query issue a GetPortStatus request and inspect the status.
			arch::dma_object<SetupPacket> statusReq{device_.setupPool()};
			statusReq->type = setup_type::targetOther | setup_type::byClass
					| setup_type::toHost;
			statusReq->request = ClassRequests::getStatus;
			statusReq->value = 0;
			statusReq->index = port + 1;
			statusReq->length = 4;

			arch::dma_array<uint16_t> result{device_.bufferPool(), 2};
			(co_await device_.transfer(ControlTransfer{kXferToHost,
					statusReq, result.view_buffer()})).unwrap();
//			std::cout << "usb: Port " << port << " status: "
//					<< result[0] << ", " << result[1] << std::endl;

			state_[port].status = 0;
			if(result[0] & PortBits::connect)
				state_[port].status |= HubStatus::connect;
			if(result[0] & PortBits::enable)
				state_[port].status |= HubStatus::enable;
			if(result[0] & PortBits::reset)
				state_[port].status |= HubStatus::reset;

			// Inspect the status change bits and reset them.
			if(result[1] & PortBits::connect) {
				state_[port].changes |= HubStatus::connect;
				doorbell_.raise();

				arch::dma_object<SetupPacket> clearReq{device_.setupPool()};
				clearReq->type = setup_type::targetOther | setup_type::byClass
						| setup_type::toDevice;
				clearReq->request = ClassRequests::clearFeature;
				clearReq->value = PortFeatures::connectChange;
				clearReq->index = port + 1;
				clearReq->length = 0;

				(co_await device_.transfer(ControlTransfer{kXferToDevice,
						clearReq, arch::dma_buffer_view{}})).unwrap();
			}

			if(result[1] & PortBits::enable) {
				state_[port].changes |= HubStatus::enable;
				doorbell_.raise();

				arch::dma_object<SetupPacket> clearReq{device_.setupPool()};
				clearReq->type = setup_type::targetOther | setup_type::byClass
						| setup_type::toDevice;
				clearReq->request = ClassRequests::clearFeature;
				clearReq->value = PortFeatures::enableChange;
				clearReq->index = port + 1;
				clearReq->length = 0;

				(co_await device_.transfer(ControlTransfer{kXferToDevice,
						clearReq, arch::dma_buffer_view{}})).unwrap();
			}

			if(result[1] & PortBits::reset) {
				state_[port].changes |= HubStatus::reset;
				doorbell_.raise();

				arch::dma_object<SetupPacket> clearReq{device_.setupPool()};
				clearReq->type = setup_type::targetOther | setup_type::byClass
						| setup_type::toDevice;
				clearReq->request = ClassRequests::clearFeature;
				clearReq->value = PortFeatures::resetChange;
				clearReq->index = port + 1;
				clearReq->length = 0;

				(co_await device_.transfer(ControlTransfer{kXferToDevice,
						clearReq, arch::dma_buffer_view{}})).unwrap();
			}
		}
	}
}

size_t StandardHub::numPorts() {
	return state_.size();
}

async::result<PortState> StandardHub::pollState(int port) {
	while(true) {
		auto state = state_[port];
		if(state.changes) {
			state_[port].changes = 0;
			co_return state;
		}

		co_await doorbell_.async_wait();
	}
}

async::result<frg::expected<UsbError, DeviceSpeed>> StandardHub::issueReset(int port) {
	// Issue a SetPortFeature request to reset the port.
	arch::dma_object<SetupPacket> resetReq{device_.setupPool()};
	resetReq->type = setup_type::targetOther | setup_type::byClass
			| setup_type::toDevice;
	resetReq->request = ClassRequests::setFeature;
	resetReq->value = PortFeatures::reset;
	resetReq->index = port + 1;
	resetReq->length = 0;

	FRG_CO_TRY(co_await device_.transfer(ControlTransfer{kXferToDevice,
			resetReq, arch::dma_buffer_view{}}));

	// Issue a GetPortStatus request to determine if the device is low-speed.
	arch::dma_object<SetupPacket> statusReq{device_.setupPool()};
	statusReq->type = setup_type::targetOther | setup_type::byClass
			| setup_type::toHost;
	statusReq->request = ClassRequests::getStatus;
	statusReq->value = 0;
	statusReq->index = port + 1;
	statusReq->length = 4;

	arch::dma_array<uint16_t> result{device_.bufferPool(), 2};
	FRG_CO_TRY(co_await device_.transfer(ControlTransfer{kXferToHost,
			statusReq, result.view_buffer()}));

	auto lowSpeed = result[0] & PortBits::lowSpeed;
	auto highSpeed = result[0] & PortBits::highSpeed;

	if (lowSpeed)
		co_return DeviceSpeed::lowSpeed;
	else if (highSpeed)
		co_return DeviceSpeed::highSpeed;
	else // TODO(qookie): What about SuperSpeed hubs?
		co_return DeviceSpeed::fullSpeed;
}

} // namespace anonymous

async::result<frg::expected<UsbError, std::shared_ptr<Hub>>>
createHubFromDevice(std::shared_ptr<Hub> parentHub, Device device, size_t port) {
	auto hub = std::make_shared<StandardHub>(parentHub, std::move(device), port);
	FRG_CO_TRY(co_await hub->initialize());
	co_return hub;
}
