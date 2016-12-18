
#include <memory>
#include <iostream>

#include <string.h>

#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <cofiber.hpp>
#include "usb.pb.h"
#include "protocols/usb/client.hpp"

namespace protocols {
namespace usb {

namespace {

struct DeviceState : DeviceData {
	DeviceState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }

	cofiber::future<std::string> configurationDescriptor() override;
	cofiber::future<Configuration> useConfiguration(int number) override;
	cofiber::future<void> transfer(ControlTransfer info) override;

private:
	helix::UniqueLane _lane;
};

struct ConfigurationState : ConfigurationData {
	ConfigurationState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }

	cofiber::future<Interface> useInterface(int number, int alternative) override;

private:
	helix::UniqueLane _lane;
};

struct InterfaceState : InterfaceData {
	InterfaceState(helix::UniqueLane lane)
	:_lane(std::move(lane)) { }

	Endpoint getEndpoint(PipeType type, int number) override;

private:
	helix::UniqueLane _lane;
};

COFIBER_ROUTINE(cofiber::future<std::string>, DeviceState::configurationDescriptor(),
		([=] {
	using M = helix::AwaitMechanism;

	helix::Offer<M> offer;
	helix::SendBuffer<M> send_req;
	helix::RecvInline<M> recv_resp;
	helix::RecvInline<M> recv_data;

	managarm::usb::CntRequest req;
	req.set_req_type(managarm::usb::CntReqType::GET_CONFIGURATION_DESCRIPTOR);

	auto ser = req.SerializeAsString();
	helix::submitAsync(_lane, {
		helix::action(&offer, kHelItemAncillary),
		helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
		helix::action(&recv_resp, kHelItemChain),
		helix::action(&recv_data)
	}, helix::Dispatcher::global());

	COFIBER_AWAIT offer.future();
	COFIBER_AWAIT send_req.future();
	COFIBER_AWAIT recv_resp.future();
	COFIBER_AWAIT recv_data.future();
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

COFIBER_ROUTINE(cofiber::future<Configuration>, DeviceState::useConfiguration(int number),
		([=] {
	using M = helix::AwaitMechanism;

	helix::Offer<M> offer;
	helix::SendBuffer<M> send_req;
	helix::RecvInline<M> recv_resp;
	helix::PullDescriptor<M> pull_lane;

	managarm::usb::CntRequest req;
	req.set_req_type(managarm::usb::CntReqType::USE_CONFIGURATION);
	req.set_number(number);

	auto ser = req.SerializeAsString();
	helix::submitAsync(_lane, {
		helix::action(&offer, kHelItemAncillary),
		helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
		helix::action(&recv_resp, kHelItemChain),
		helix::action(&pull_lane),
	}, helix::Dispatcher::global());
	
	COFIBER_AWAIT offer.future();
	COFIBER_AWAIT send_req.future();
	COFIBER_AWAIT recv_resp.future();
	COFIBER_AWAIT pull_lane.future();
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

COFIBER_ROUTINE(cofiber::future<void>, DeviceState::transfer(ControlTransfer info),
		([=] {
	using M = helix::AwaitMechanism;

	if(info.flags == kXferToDevice) {
		throw std::runtime_error("xfer to device not implemented");
	}else{
		assert(info.flags == kXferToHost);
	
		helix::Offer<M> offer;
		helix::SendBuffer<M> send_req;
		helix::RecvInline<M> recv_resp;
		helix::RecvBuffer<M> recv_data;

		managarm::usb::CntRequest req;
		req.set_req_type(managarm::usb::CntReqType::TRANSFER_TO_HOST);
		req.set_recipient(info.recipient);
		req.set_type(info.type);
		req.set_request(info.request);
		req.set_arg0(info.arg0);
		req.set_arg1(info.arg1);
		req.set_length(info.length);

		auto ser = req.SerializeAsString();
		helix::submitAsync(_lane, {
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, kHelItemChain),
			helix::action(&recv_data, info.buffer, info.length)
		}, helix::Dispatcher::global());

		COFIBER_AWAIT offer.future();
		COFIBER_AWAIT send_req.future();
		COFIBER_AWAIT recv_resp.future();
		COFIBER_AWAIT recv_data.future();
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

COFIBER_ROUTINE(cofiber::future<Interface>, ConfigurationState::useInterface(int number, int alternative),
		([=] {
	using M = helix::AwaitMechanism;
	
	helix::Offer<M> offer;
	helix::SendBuffer<M> send_req;
	helix::RecvInline<M> recv_resp;
	helix::PullDescriptor<M> pull_lane;

	managarm::usb::CntRequest req;
	req.set_req_type(managarm::usb::CntReqType::USE_INTERFACE);
	req.set_number(number);
	req.set_alternative(alternative);

	auto ser = req.SerializeAsString();
	helix::submitAsync(_lane, {
		helix::action(&offer, kHelItemAncillary),
		helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
		helix::action(&recv_resp, kHelItemChain),
		helix::action(&pull_lane),
	}, helix::Dispatcher::global());
	
	COFIBER_AWAIT offer.future();
	COFIBER_AWAIT send_req.future();
	COFIBER_AWAIT recv_resp.future();
	COFIBER_AWAIT pull_lane.future();
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

Endpoint InterfaceState::getEndpoint(PipeType type, int number) {
	throw std::runtime_error("get endpoint");
}

} // anonymous namespace

Device connect(helix::UniqueLane lane) {
	return Device(std::make_shared<DeviceState>(std::move(lane)));
}

} } // namespace protocols::usb

