
#include <memory>
#include <iostream>

#include <string.h>

#include <async/result.hpp>
#include <helix/ipc.hpp>

#include "usb.pb.h"
#include "protocols/usb/client.hpp"

namespace protocols::usb {

namespace {

struct DeviceState final : DeviceData {
	DeviceState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }

	arch::dma_pool *setupPool() override;
	arch::dma_pool *bufferPool() override;

	async::result<frg::expected<UsbError, std::string>> configurationDescriptor() override;
	async::result<frg::expected<UsbError, Configuration>> useConfiguration(int number) override;
	async::result<frg::expected<UsbError>> transfer(ControlTransfer info) override;

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
	InterfaceState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }

	async::result<frg::expected<UsbError, Endpoint>>
	getEndpoint(PipeType type, int number) override;

private:
	helix::UniqueLane _lane;
};


struct EndpointState final : EndpointData {
	EndpointState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }
	
	async::result<frg::expected<UsbError>> transfer(ControlTransfer info) override;
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

async::result<frg::expected<UsbError, std::string>> DeviceState::configurationDescriptor() {
	managarm::usb::CntRequest req;
	req.set_req_type(managarm::usb::CntReqType::GET_CONFIGURATION_DESCRIPTOR);

	auto ser = req.SerializeAsString();

	auto [offer, sendReq, recvResp, recvData] = co_await helix_ng::exchangeMsgs(
				_lane,
				helix_ng::offer(
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::recvInline(),
					helix_ng::recvInline()
				)
			);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	managarm::usb::SvrResponse resp;
	resp.ParseFromArray(recvResp.data(), recvResp.length());

	FRG_CO_TRY(transformProtocolError(resp.error()));

	HEL_CHECK(recvData.error());

	std::string data(recvData.length(), 0);
	memcpy(&data[0], recvData.data(), recvData.length());
	co_return std::move(data);
}

async::result<frg::expected<UsbError, Configuration>> DeviceState::useConfiguration(int number) {
	managarm::usb::CntRequest req;
	req.set_req_type(managarm::usb::CntReqType::USE_CONFIGURATION);
	req.set_number(number);

	auto ser = req.SerializeAsString();

	auto [offer, sendReq, recvResp, pullLane] = co_await helix_ng::exchangeMsgs(
				_lane,
				helix_ng::offer(
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::recvInline(),
					helix_ng::pullDescriptor()
				)
			);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	managarm::usb::SvrResponse resp;
	resp.ParseFromArray(recvResp.data(), recvResp.length());

	FRG_CO_TRY(transformProtocolError(resp.error()));

	HEL_CHECK(pullLane.error());

	auto state = std::make_shared<ConfigurationState>(pullLane.descriptor());
	co_return Configuration(std::move(state));
}

async::result<frg::expected<UsbError>> DeviceState::transfer(ControlTransfer info) {
	if(info.flags == kXferToDevice) {
		throw std::runtime_error("xfer to device not implemented");
	}else{
		assert(info.flags == kXferToHost);

		managarm::usb::CntRequest req;
		req.set_req_type(managarm::usb::CntReqType::TRANSFER_TO_HOST);
		req.set_length(info.buffer.size());

		auto ser = req.SerializeAsString();

		auto [offer, sendReq, sendSetup, recvResp, recvData] = co_await helix_ng::exchangeMsgs(
					_lane,
					helix_ng::offer(
						helix_ng::sendBuffer(ser.data(), ser.size()),
						helix_ng::sendBuffer(info.setup.data(), sizeof(SetupPacket)),
						helix_ng::recvInline(),
						helix_ng::recvBuffer(info.buffer.data(), info.buffer.size())
					)
				);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(sendSetup.error());
		HEL_CHECK(recvResp.error());

		managarm::usb::SvrResponse resp;
		resp.ParseFromArray(recvResp.data(), recvResp.length());

		FRG_CO_TRY(transformProtocolError(resp.error()));

		HEL_CHECK(recvData.error());

		co_return {};
	}
}

async::result<frg::expected<UsbError, Interface>>
ConfigurationState::useInterface(int number, int alternative) {
	managarm::usb::CntRequest req;
	req.set_req_type(managarm::usb::CntReqType::USE_INTERFACE);
	req.set_number(number);
	req.set_alternative(alternative);

	auto ser = req.SerializeAsString();

	auto [offer, sendReq, recvResp, pullLane] = co_await helix_ng::exchangeMsgs(
				_lane,
				helix_ng::offer(
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::recvInline(),
					helix_ng::pullDescriptor()
				)
			);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	managarm::usb::SvrResponse resp;
	resp.ParseFromArray(recvResp.data(), recvResp.length());

	FRG_CO_TRY(transformProtocolError(resp.error()));

	HEL_CHECK(pullLane.error());

	auto state = std::make_shared<InterfaceState>(pullLane.descriptor());
	co_return Interface(std::move(state));
}

async::result<frg::expected<UsbError, Endpoint>>
InterfaceState::getEndpoint(PipeType type, int number) {
	managarm::usb::CntRequest req;
	req.set_req_type(managarm::usb::CntReqType::GET_ENDPOINT);
	req.set_pipetype(static_cast<int>(type));
	req.set_number(number);

	auto ser = req.SerializeAsString();

	auto [offer, sendReq, recvResp, pullLane] = co_await helix_ng::exchangeMsgs(
				_lane,
				helix_ng::offer(
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::recvInline(),
					helix_ng::pullDescriptor()
				)
			);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());

	managarm::usb::SvrResponse resp;
	resp.ParseFromArray(recvResp.data(), recvResp.length());

	FRG_CO_TRY(transformProtocolError(resp.error()));

	HEL_CHECK(pullLane.error());

	auto state = std::make_shared<EndpointState>(pullLane.descriptor());
	co_return Endpoint(std::move(state));
}

async::result<frg::expected<UsbError>> EndpointState::transfer(ControlTransfer) {
	throw std::runtime_error("endpoint control transfer not implemented");
}

async::result<frg::expected<UsbError, size_t>> EndpointState::transfer(InterruptTransfer info) {
	if(info.flags == kXferToDevice) {
		throw std::runtime_error("xfer to device not implemented");
	}else{
		assert(info.flags == kXferToHost);

		managarm::usb::CntRequest req;
		req.set_req_type(managarm::usb::CntReqType::INTERRUPT_TRANSFER_TO_HOST);
		req.set_length(info.buffer.size());
		req.set_allow_short(info.allowShortPackets);
		req.set_lazy_notification(info.lazyNotification);

		auto ser = req.SerializeAsString();

		auto [offer, sendReq, recvResp, recvData] = co_await helix_ng::exchangeMsgs(
					_lane,
					helix_ng::offer(
						helix_ng::sendBuffer(ser.data(), ser.size()),
						helix_ng::recvInline(),
						helix_ng::recvBuffer(info.buffer.data(), info.buffer.size())
					)
				);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(recvResp.error());

		managarm::usb::SvrResponse resp;
		resp.ParseFromArray(recvResp.data(), recvResp.length());

		FRG_CO_TRY(transformProtocolError(resp.error()));

		HEL_CHECK(recvData.error());

		co_return recvData.actualLength();
	}
}

async::result<frg::expected<UsbError, size_t>> EndpointState::transfer(BulkTransfer info) {
	if(info.flags == kXferToDevice) {
		assert(info.flags == kXferToDevice);
		assert(!info.allowShortPackets);

		managarm::usb::CntRequest req;
		req.set_req_type(managarm::usb::CntReqType::BULK_TRANSFER_TO_DEVICE);
		req.set_length(info.buffer.size());
		req.set_lazy_notification(info.lazyNotification);

		auto ser = req.SerializeAsString();

		auto [offer, sendReq, sendData, recvResp] = co_await helix_ng::exchangeMsgs(
					_lane,
					helix_ng::offer(
						helix_ng::sendBuffer(ser.data(), ser.size()),
						helix_ng::sendBuffer(info.buffer.data(), info.buffer.size()),
						helix_ng::recvInline()
					)
				);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(sendData.error());
		HEL_CHECK(recvResp.error());

		managarm::usb::SvrResponse resp;
		resp.ParseFromArray(recvResp.data(), recvResp.length());

		FRG_CO_TRY(transformProtocolError(resp.error()));

		co_return resp.size();
	}else{
		assert(info.flags == kXferToHost);

		managarm::usb::CntRequest req;
		req.set_req_type(managarm::usb::CntReqType::BULK_TRANSFER_TO_HOST);
		req.set_length(info.buffer.size());
		req.set_allow_short(info.allowShortPackets);
		req.set_lazy_notification(info.lazyNotification);

		auto ser = req.SerializeAsString();

		auto [offer, sendReq, recvResp, recvData] = co_await helix_ng::exchangeMsgs(
					_lane,
					helix_ng::offer(
						helix_ng::sendBuffer(ser.data(), ser.size()),
						helix_ng::recvInline(),
						helix_ng::recvBuffer(info.buffer.data(), info.buffer.size())
					)
				);

		HEL_CHECK(offer.error());
		HEL_CHECK(sendReq.error());
		HEL_CHECK(recvResp.error());

		managarm::usb::SvrResponse resp;
		resp.ParseFromArray(recvResp.data(), recvResp.length());

		FRG_CO_TRY(transformProtocolError(resp.error()));

		HEL_CHECK(recvData.error());

		co_return recvData.actualLength();
	}
}

} // anonymous namespace

Device connect(helix::UniqueLane lane) {
	return Device(std::make_shared<DeviceState>(std::move(lane)));
}

} // namespace protocols::usb

