
#include <memory>
#include <iostream>

#include <string.h>

#include <async/result.hpp>
#include <helix/ipc.hpp>

#include <bragi/helpers-std.hpp>
#include "usb.bragi.hpp"
#include "protocols/usb/client.hpp"

namespace protocols::usb {

namespace {

struct DeviceState final : DeviceData {
	DeviceState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }

	arch::dma_pool *setupPool() override;
	arch::dma_pool *bufferPool() override;

	async::result<frg::expected<UsbError, std::string>> deviceDescriptor() override;
	async::result<frg::expected<UsbError, std::string>> configurationDescriptor(uint8_t configuration) override;
	async::result<frg::expected<UsbError, Configuration>> useConfiguration(uint8_t index, uint8_t value) override;
	async::result<frg::expected<UsbError, size_t>> transfer(ControlTransfer info) override;

private:
	helix::UniqueLane _lane;
};

struct ConfigurationState final : ConfigurationData {
	ConfigurationState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }

	async::result<frg::expected<UsbError, Interface>>
	useInterface(int number, int alternative) override;

private:
	helix::UniqueLane _lane;
};

struct InterfaceState final : InterfaceData {
	InterfaceState(int num, helix::UniqueLane lane)
	: InterfaceData{num}, _lane(std::move(lane)) { }

	async::result<frg::expected<UsbError, Endpoint>>
	getEndpoint(PipeType type, int number) override;

private:
	helix::UniqueLane _lane;
};


struct EndpointState final : EndpointData {
	EndpointState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }

	async::result<frg::expected<UsbError, size_t>> transfer(ControlTransfer info) override;
	async::result<frg::expected<UsbError, size_t>> transfer(InterruptTransfer info) override;
	async::result<frg::expected<UsbError, size_t>> transfer(BulkTransfer info) override;

private:
	helix::UniqueLane _lane;
};

arch::dma_pool *DeviceState::setupPool() {
	return nullptr;
}

arch::dma_pool *DeviceState::bufferPool() {
	return nullptr;
}

frg::expected<UsbError> transformProtocolError(managarm::usb::Errors error) {
	switch (error) {
		using enum UsbError;
		using enum managarm::usb::Errors;

		case SUCCESS: return frg::success;
		case STALL: return stall;
		case BABBLE: return babble;
		case TIMEOUT: return timeout;
		case UNSUPPORTED: return unsupported;
		case OTHER: return other;
		case ILLEGAL_REQUEST: assert(!"Illegal request in USB client"); break;
		default: assert(!"Invalid error code in protocolErrorIntoApiError");
	}

	return UsbError::other;
}

async::result<frg::expected<UsbError, std::string>> DeviceState::deviceDescriptor() {
	managarm::usb::GetDeviceDescriptorRequest req;

	auto [offer, sendReq, recvResp, recvData] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::recvInline()
		)
	);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto resp = bragi::parse_head_only<managarm::usb::SvrResponse>(recvResp);

	FRG_CO_TRY(transformProtocolError(resp->error()));

	HEL_CHECK(recvData.error());

	std::string data(recvData.length(), 0);
	memcpy(&data[0], recvData.data(), recvData.length());
	co_return std::move(data);
}

async::result<frg::expected<UsbError, std::string>> DeviceState::configurationDescriptor(uint8_t configuration) {
	managarm::usb::GetConfigurationDescriptorRequest req;
	req.set_configuration(configuration);

	auto [offer, sendReq, recvResp] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
			helix_ng::want_lane,
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline()
		)
	);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto resp = bragi::parse_head_only<managarm::usb::SvrResponse>(recvResp);

	std::string recvBuffer(resp->size(), 0);
	auto [recvData] = co_await helix_ng::exchangeMsgs(
		offer.descriptor(),
		helix_ng::recvBuffer(recvBuffer.data(), recvBuffer.size())
	);

	FRG_CO_TRY(transformProtocolError(resp->error()));

	HEL_CHECK(recvData.error());

	co_return recvBuffer;
}

async::result<frg::expected<UsbError, Configuration>> DeviceState::useConfiguration(uint8_t index, uint8_t value) {
	managarm::usb::UseConfigurationRequest req;
	req.set_index(index);
	req.set_value(value);

	auto [offer, sendReq, recvResp, pullLane] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor()
		)
	);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto resp = bragi::parse_head_only<managarm::usb::SvrResponse>(recvResp);

	FRG_CO_TRY(transformProtocolError(resp->error()));

	HEL_CHECK(pullLane.error());

	auto state = std::make_shared<ConfigurationState>(pullLane.descriptor());
	co_return Configuration(std::move(state));
}

async::result<frg::expected<UsbError, size_t>>
doControlTransfer(auto &lane, ControlTransfer info) {
	managarm::usb::TransferRequest req;

	req.set_type(managarm::usb::XferType::CONTROL);
	req.set_dir(info.flags == kXferToDevice
		    ? managarm::usb::XferDirection::TO_DEVICE
		    : managarm::usb::XferDirection::TO_HOST);

	req.set_length(info.buffer.size());

	if(info.flags == kXferToDevice) {
		auto [offer, sendReq, sendSetup, sendData, recvResp] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::sendBuffer(info.setup.data(), sizeof(SetupPacket)),
				helix_ng::sendBuffer(info.buffer.data(), info.buffer.size()),
				helix_ng::recvInline()
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(sendSetup.error());
		HEL_CHECK(sendData.error());
		HEL_CHECK(recvResp.error());

		auto resp = bragi::parse_head_only<managarm::usb::SvrResponse>(recvResp);

		FRG_CO_TRY(transformProtocolError(resp->error()));

		co_return 0;
	}else{
		auto [offer, sendReq, sendSetup, recvResp, recvData] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::sendBuffer(info.setup.data(), sizeof(SetupPacket)),
				helix_ng::recvInline(),
				helix_ng::recvBuffer(info.buffer.data(), info.buffer.size())
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(sendSetup.error());
		HEL_CHECK(recvResp.error());

		auto resp = bragi::parse_head_only<managarm::usb::SvrResponse>(recvResp);

		FRG_CO_TRY(transformProtocolError(resp->error()));

		HEL_CHECK(recvData.error());

		co_return resp->size();
	}
}

async::result<frg::expected<UsbError, size_t>> DeviceState::transfer(ControlTransfer info) {
	co_return co_await doControlTransfer(_lane, info);
}

async::result<frg::expected<UsbError, Interface>>
ConfigurationState::useInterface(int number, int alternative) {
	managarm::usb::UseInterfaceRequest req;

	req.set_number(number);
	req.set_alternative(alternative);

	auto [offer, sendReq, recvResp, pullLane] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor()
		)
	);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto resp = bragi::parse_head_only<managarm::usb::SvrResponse>(recvResp);

	FRG_CO_TRY(transformProtocolError(resp->error()));

	HEL_CHECK(pullLane.error());

	auto state = std::make_shared<InterfaceState>(number, pullLane.descriptor());
	co_return Interface(std::move(state));
}

async::result<frg::expected<UsbError, Endpoint>>
InterfaceState::getEndpoint(PipeType type, int number) {
	managarm::usb::GetEndpointRequest req;

	req.set_type(static_cast<int>(type));
	req.set_number(number);

	auto [offer, sendReq, recvResp, pullLane] = co_await helix_ng::exchangeMsgs(
		_lane,
		helix_ng::offer(
			helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
			helix_ng::recvInline(),
			helix_ng::pullDescriptor()
		)
	);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	auto resp = bragi::parse_head_only<managarm::usb::SvrResponse>(recvResp);

	FRG_CO_TRY(transformProtocolError(resp->error()));

	HEL_CHECK(pullLane.error());

	auto state = std::make_shared<EndpointState>(pullLane.descriptor());
	co_return Endpoint(std::move(state));
}

template <typename XferInfo>
async::result<frg::expected<UsbError, size_t>>
doTransferOfType(auto &lane, managarm::usb::XferType type, XferInfo info) {
	managarm::usb::TransferRequest req;

	req.set_type(type);
	req.set_dir(info.flags == kXferToDevice
		    ? managarm::usb::XferDirection::TO_DEVICE
		    : managarm::usb::XferDirection::TO_HOST);

	req.set_allow_short_packets(info.allowShortPackets);
	req.set_lazy_notification(info.lazyNotification);
	req.set_length(info.buffer.size());

	if(info.flags == kXferToDevice) {
		auto [offer, sendReq, sendData, recvResp] =
			co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::offer(
					helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
					helix_ng::sendBuffer(info.buffer.data(), info.buffer.size()),
					helix_ng::recvInline()
				)
			);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(sendData.error());
		HEL_CHECK(recvResp.error());

		auto resp = bragi::parse_head_only<managarm::usb::SvrResponse>(recvResp);

		FRG_CO_TRY(transformProtocolError(resp->error()));

		co_return resp->size();
	}else{
		auto [offer, sendReq, recvResp, recvData] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::recvBuffer(info.buffer.data(), info.buffer.size())
			)
		);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(recvResp.error());

		auto resp = bragi::parse_head_only<managarm::usb::SvrResponse>(recvResp);

		FRG_CO_TRY(transformProtocolError(resp->error()));

		HEL_CHECK(recvData.error());

		co_return recvData.actualLength();
	}
}

async::result<frg::expected<UsbError, size_t>> EndpointState::transfer(ControlTransfer info) {
	co_return co_await doControlTransfer(_lane, info);
}

async::result<frg::expected<UsbError, size_t>> EndpointState::transfer(InterruptTransfer info) {
	co_return co_await doTransferOfType(_lane, managarm::usb::XferType::INTERRUPT, info);
}

async::result<frg::expected<UsbError, size_t>> EndpointState::transfer(BulkTransfer info) {
	co_return co_await doTransferOfType(_lane, managarm::usb::XferType::BULK, info);
}

} // anonymous namespace

Device connect(helix::UniqueLane lane) {
	return Device(std::make_shared<DeviceState>(std::move(lane)));
}

} // namespace protocols::usb

