
#include <memory>
#include <iostream>

#include <string.h>

#include <async/result.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>

#include "usb.pb.h"
#include "protocols/usb/client.hpp"

namespace protocols {
namespace usb {

namespace {

struct DeviceState : DeviceData {
	DeviceState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }

	arch::dma_pool *setupPool() override;
	arch::dma_pool *bufferPool() override;

	async::result<std::string> configurationDescriptor() override;
	async::result<Configuration> useConfiguration(int number) override;
	async::result<void> transfer(ControlTransfer info) override;

private:
	helix::UniqueLane _lane;
};

struct ConfigurationState : ConfigurationData {
	ConfigurationState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }

	async::result<Interface> useInterface(int number, int alternative) override;

private:
	helix::UniqueLane _lane;
};

struct InterfaceState : InterfaceData {
	InterfaceState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }

	async::result<Endpoint> getEndpoint(PipeType type, int number) override;

private:
	helix::UniqueLane _lane;
};


struct EndpointState : EndpointData {
	EndpointState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }
	
	async::result<void> transfer(ControlTransfer info) override;
	async::result<void> transfer(InterruptTransfer info) override;
	async::result<void> transfer(BulkTransfer info) override;

private:
	helix::UniqueLane _lane;
};

arch::dma_pool *DeviceState::setupPool() {
	return nullptr;
}

arch::dma_pool *DeviceState::bufferPool() {
	return nullptr;
}

COFIBER_ROUTINE(async::result<std::string>, DeviceState::configurationDescriptor(),
		([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;
	helix::RecvInline recv_data;

	managarm::usb::CntRequest req;
	req.set_req_type(managarm::usb::CntReqType::GET_CONFIGURATION_DESCRIPTOR);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&recv_data));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(recv_data.error());

	managarm::usb::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::usb::Errors::SUCCESS);

	std::string data(recv_data.length(), 0);
	memcpy(&data[0], recv_data.data(), recv_data.length());
	COFIBER_RETURN(std::move(data));
}))

COFIBER_ROUTINE(async::result<Configuration>, DeviceState::useConfiguration(int number),
		([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;
	helix::PullDescriptor pull_lane;

	managarm::usb::CntRequest req;
	req.set_req_type(managarm::usb::CntReqType::USE_CONFIGURATION);
	req.set_number(number);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&pull_lane));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_lane.error());

	managarm::usb::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::usb::Errors::SUCCESS);

	auto state = std::make_shared<ConfigurationState>(pull_lane.descriptor());
	COFIBER_RETURN(Configuration(std::move(state)));
}))

COFIBER_ROUTINE(async::result<void>, DeviceState::transfer(ControlTransfer info),
		([=] {
	if(info.flags == kXferToDevice) {
		throw std::runtime_error("xfer to device not implemented");
	}else{
		assert(info.flags == kXferToHost);
	
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::SendBuffer send_setup;
		helix::RecvInline recv_resp;
		helix::RecvBuffer recv_data;

		managarm::usb::CntRequest req;
		req.set_req_type(managarm::usb::CntReqType::TRANSFER_TO_HOST);
		req.set_length(info.buffer.size());
		
		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&send_setup, info.setup.data(), sizeof(SetupPacket), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&recv_data, info.buffer.data(), info.buffer.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_setup.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(recv_data.error());

		managarm::usb::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::usb::Errors::SUCCESS);
		COFIBER_RETURN();
	}
}))

COFIBER_ROUTINE(async::result<Interface>, ConfigurationState::useInterface(int number,
		int alternative), ([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;
	helix::PullDescriptor pull_lane;

	managarm::usb::CntRequest req;
	req.set_req_type(managarm::usb::CntReqType::USE_INTERFACE);
	req.set_number(number);
	req.set_alternative(alternative);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&pull_lane));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_lane.error());
	
	managarm::usb::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::usb::Errors::SUCCESS);

	auto state = std::make_shared<InterfaceState>(pull_lane.descriptor());
	COFIBER_RETURN(Interface(std::move(state)));
}))

COFIBER_ROUTINE(async::result<Endpoint>, InterfaceState::getEndpoint(PipeType type, int number),
		([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvInline recv_resp;
	helix::PullDescriptor pull_lane;

	managarm::usb::CntRequest req;
	req.set_req_type(managarm::usb::CntReqType::GET_ENDPOINT);
	req.set_pipetype(static_cast<int>(type));
	req.set_number(number);

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&pull_lane));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_lane.error());
	
	managarm::usb::SvrResponse resp;
	resp.ParseFromArray(recv_resp.data(), recv_resp.length());
	assert(resp.error() == managarm::usb::Errors::SUCCESS);

	auto state = std::make_shared<EndpointState>(pull_lane.descriptor());
	COFIBER_RETURN(Endpoint(std::move(state)));
}))

COFIBER_ROUTINE(async::result<void>, EndpointState::transfer(ControlTransfer info),
		([=] {
	throw std::runtime_error("endpoint control transfer not implemented");
}))

COFIBER_ROUTINE(async::result<void>, EndpointState::transfer(InterruptTransfer info),
		([=] {
	if(info.flags == kXferToDevice) {
		throw std::runtime_error("xfer to device not implemented");
	}else{
		assert(info.flags == kXferToHost);
	
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::RecvBuffer recv_data;

		managarm::usb::CntRequest req;
		req.set_req_type(managarm::usb::CntReqType::INTERRUPT_TRANSFER_TO_HOST);
		req.set_length(info.buffer.size());

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&recv_data, info.buffer.data(), info.buffer.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(recv_data.error());

		managarm::usb::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::usb::Errors::SUCCESS);
		COFIBER_RETURN();
	}
}))

COFIBER_ROUTINE(async::result<void>, EndpointState::transfer(BulkTransfer info),
		([=] {
	if(info.flags == kXferToDevice) {
		assert(info.flags == kXferToDevice);
	
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::SendBuffer send_data;
		helix::RecvInline recv_resp;

		managarm::usb::CntRequest req;
		req.set_req_type(managarm::usb::CntReqType::BULK_TRANSFER_TO_DEVICE);
		req.set_length(info.buffer.size());
		
		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&send_data, info.buffer.data(), info.buffer.size(), kHelItemChain),
				helix::action(&recv_resp));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(send_data.error());
		HEL_CHECK(recv_resp.error());

		managarm::usb::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::usb::Errors::SUCCESS);
		COFIBER_RETURN();
	}else{
		assert(info.flags == kXferToHost);
	
		helix::Offer offer;
		helix::SendBuffer send_req;
		helix::RecvInline recv_resp;
		helix::RecvBuffer recv_data;

		managarm::usb::CntRequest req;
		req.set_req_type(managarm::usb::CntReqType::BULK_TRANSFER_TO_HOST);
		req.set_length(info.buffer.size());
		
		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
				helix::action(&offer, kHelItemAncillary),
				helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
				helix::action(&recv_resp, kHelItemChain),
				helix::action(&recv_data, info.buffer.data(), info.buffer.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(offer.error());
		HEL_CHECK(send_req.error());
		HEL_CHECK(recv_resp.error());
		HEL_CHECK(recv_data.error());

		managarm::usb::SvrResponse resp;
		resp.ParseFromArray(recv_resp.data(), recv_resp.length());
		assert(resp.error() == managarm::usb::Errors::SUCCESS);
		COFIBER_RETURN();
	}
}))


} // anonymous namespace

Device connect(helix::UniqueLane lane) {
	return Device(std::make_shared<DeviceState>(std::move(lane)));
}

} } // namespace protocols::usb

