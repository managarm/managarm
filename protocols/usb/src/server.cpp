
#include <string.h>
#include <iostream>

#include <helix/await.hpp>
#include "protocols/usb/server.hpp"
#include "usb.pb.h"

namespace protocols {
namespace usb {

COFIBER_ROUTINE(cofiber::no_future, serveEndpoint(Endpoint endpoint,
		helix::UniqueLane p), ([endpoint, lane = std::move(p)] () {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
	
		if(req.req_type() == managarm::usb::CntReqType::INTERRUPT_TRANSFER_TO_HOST) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;

			// FIXME: Fill in the correct DMA pool.
			arch::dma_buffer buffer{nullptr, req.length()};
			InterruptTransfer transfer{XferFlags::kXferToHost, buffer};
			transfer.allowShortPackets = req.allow_short();
			auto length = COFIBER_AWAIT endpoint.transfer(transfer);

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, buffer.data(), length));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else if(req.req_type() == managarm::usb::CntReqType::BULK_TRANSFER_TO_DEVICE) {
			helix::RecvBuffer recv_buffer;
			helix::SendBuffer send_resp;
		
			// FIXME: Fill in the correct DMA pool.
			arch::dma_buffer buffer{nullptr, req.length()};
			auto &&payload = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&recv_buffer, buffer.data(), buffer.size()));
			COFIBER_AWAIT payload.async_wait();
			HEL_CHECK(recv_buffer.error());
		
			BulkTransfer transfer{XferFlags::kXferToDevice, buffer};
			auto length = COFIBER_AWAIT endpoint.transfer(transfer);

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);
			resp.set_size(length);
			
			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.req_type() == managarm::usb::CntReqType::BULK_TRANSFER_TO_HOST) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;

			// FIXME: Fill in the correct DMA pool.
			arch::dma_buffer buffer{nullptr, req.length()};
			BulkTransfer transfer{XferFlags::kXferToHost, buffer};
			transfer.allowShortPackets = req.allow_short();
			auto length = COFIBER_AWAIT endpoint.transfer(transfer);

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, buffer.data(), length));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else{
			helix::SendBuffer send_resp;

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}))

COFIBER_ROUTINE(cofiber::no_future, serveInterface(Interface interface,
		helix::UniqueLane p), ([interface, lane = std::move(p)] () {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
	
		if(req.req_type() == managarm::usb::CntReqType::GET_ENDPOINT) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_lane;

			auto endpoint = COFIBER_AWAIT interface.getEndpoint(static_cast<PipeType>(req.pipetype()), req.number());
			
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			
			serveEndpoint(std::move(endpoint), std::move(local_lane));

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_lane, remote_lane));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_lane.error());
		}else {
			helix::SendBuffer send_resp;

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}))

COFIBER_ROUTINE(cofiber::no_future, serveConfiguration(Configuration configuration,
		helix::UniqueLane p), ([configuration, lane = std::move(p)] () {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		
		if(req.req_type() == managarm::usb::CntReqType::USE_INTERFACE) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_lane;

			auto interface = COFIBER_AWAIT configuration.useInterface(req.number(),
					req.alternative());
			
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			serveInterface(std::move(interface), std::move(local_lane));

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_lane, remote_lane));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_lane.error());
		}else {
			helix::SendBuffer send_resp;

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}))

COFIBER_ROUTINE(cofiber::no_future, serve(Device device, helix::UniqueLane p),
		([device, lane = std::move(p)] () {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		
		if(req.req_type() == managarm::usb::CntReqType::GET_CONFIGURATION_DESCRIPTOR) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;

			auto data = COFIBER_AWAIT device.configurationDescriptor();

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, data.data(), data.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else if(req.req_type() == managarm::usb::CntReqType::TRANSFER_TO_HOST) {
			helix::RecvBuffer recv_buffer;
			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;
			
			arch::dma_object<SetupPacket> setup(nullptr);
			auto &&payload = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&recv_buffer, setup.data(), sizeof(SetupPacket)));
			COFIBER_AWAIT payload.async_wait();
			HEL_CHECK(recv_buffer.error());
			arch::dma_buffer buffer{nullptr, req.length()};
			COFIBER_AWAIT device.transfer(ControlTransfer{XferFlags::kXferToHost, setup, buffer});	

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, buffer.data(), buffer.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else if(req.req_type() == managarm::usb::CntReqType::USE_CONFIGURATION) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_lane;

			auto configuration = COFIBER_AWAIT device.useConfiguration(req.number());
			
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			serveConfiguration(std::move(configuration), std::move(local_lane));

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_lane, remote_lane));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_lane.error());
		}else {
			helix::SendBuffer send_resp;

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}))

}}

