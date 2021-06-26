
#include <string.h>
#include <iostream>

#include "protocols/usb/server.hpp"
#include "usb.pb.h"

namespace protocols {
namespace usb {

async::detached serveEndpoint(Endpoint endpoint, helix::UniqueLane lane) {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		co_await header.async_wait();
		if(accept.error() == kHelErrEndOfLane)
			co_return;

		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());

		if(req.req_type() == managarm::usb::CntReqType::INTERRUPT_TRANSFER_TO_HOST) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;

			// FIXME: Fill in the correct DMA pool.
			arch::dma_buffer buffer{nullptr, static_cast<size_t>(req.length())};
			InterruptTransfer transfer{XferFlags::kXferToHost, buffer};
			transfer.allowShortPackets = req.allow_short();
			transfer.lazyNotification = req.lazy_notification();
			auto outcome = co_await endpoint.transfer(transfer);
			assert(outcome);
			auto length = outcome.value();

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, buffer.data(), length));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else if(req.req_type() == managarm::usb::CntReqType::BULK_TRANSFER_TO_DEVICE) {
			helix::RecvBuffer recv_buffer;
			helix::SendBuffer send_resp;

			// FIXME: Fill in the correct DMA pool.
			arch::dma_buffer buffer{nullptr, static_cast<size_t>(req.length())};
			auto &&payload = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&recv_buffer, buffer.data(), buffer.size()));
			co_await payload.async_wait();
			HEL_CHECK(recv_buffer.error());

			BulkTransfer transfer{XferFlags::kXferToDevice, buffer};
			transfer.lazyNotification = req.lazy_notification();
			auto outcome = co_await endpoint.transfer(transfer);
			assert(outcome);
			auto length = outcome.value();

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);
			resp.set_size(length);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.req_type() == managarm::usb::CntReqType::BULK_TRANSFER_TO_HOST) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;

			// FIXME: Fill in the correct DMA pool.
			arch::dma_buffer buffer{nullptr, static_cast<size_t>(req.length())};
			BulkTransfer transfer{XferFlags::kXferToHost, buffer};
			transfer.allowShortPackets = req.allow_short();
			transfer.lazyNotification = req.lazy_notification();
			auto outcome = co_await endpoint.transfer(transfer);
			assert(outcome);
			auto length = outcome.value();

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, buffer.data(), length));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else{
			helix::SendBuffer send_resp;

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}

async::detached serveInterface(Interface interface, helix::UniqueLane lane) {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		co_await header.async_wait();
		if(accept.error() == kHelErrEndOfLane)
			co_return;

		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());

		if(req.req_type() == managarm::usb::CntReqType::GET_ENDPOINT) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_lane;

			auto outcome = co_await interface.getEndpoint(static_cast<PipeType>(req.pipetype()),
					req.number());
			assert(outcome);
			auto endpoint = std::move(outcome.value());

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();

			serveEndpoint(std::move(endpoint), std::move(local_lane));

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_lane, remote_lane));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_lane.error());
		}else {
			helix::SendBuffer send_resp;

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}

async::detached serveConfiguration(Configuration configuration, helix::UniqueLane lane) {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		co_await header.async_wait();
		if(accept.error() == kHelErrEndOfLane)
			co_return;

		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());

		if(req.req_type() == managarm::usb::CntReqType::USE_INTERFACE) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_lane;

			auto outcome = co_await configuration.useInterface(req.number(),
					req.alternative());
			assert(outcome);
			auto interface = std::move(outcome.value());

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			serveInterface(std::move(interface), std::move(local_lane));

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_lane, remote_lane));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_lane.error());
		}else {
			helix::SendBuffer send_resp;

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}

async::detached serve(Device device, helix::UniqueLane lane) {
	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		co_await header.async_wait();
		if(accept.error() == kHelErrEndOfLane)
			co_return;

		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());

		if(req.req_type() == managarm::usb::CntReqType::GET_CONFIGURATION_DESCRIPTOR) {
			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;

			auto outcome = co_await device.configurationDescriptor();
			assert(outcome);
			auto data = std::move(outcome.value());

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, data.data(), data.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else if(req.req_type() == managarm::usb::CntReqType::TRANSFER_TO_HOST) {
			helix::RecvBuffer recv_buffer;
			helix::SendBuffer send_resp;
			helix::SendBuffer send_data;

			arch::dma_object<SetupPacket> setup(nullptr);
			auto &&payload = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&recv_buffer, setup.data(), sizeof(SetupPacket)));
			co_await payload.async_wait();
			HEL_CHECK(recv_buffer.error());
			arch::dma_buffer buffer{nullptr, static_cast<size_t>(req.length())};
			auto outcome = co_await device.transfer(ControlTransfer{
					XferFlags::kXferToHost, setup, buffer});
			assert(outcome);

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_data, buffer.data(), buffer.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_data.error());
		}else if(req.req_type() == managarm::usb::CntReqType::USE_CONFIGURATION) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_lane;

			auto outcome = co_await device.useConfiguration(req.number());
			assert(outcome);
			auto configuration = std::move(outcome.value());

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			serveConfiguration(std::move(configuration), std::move(local_lane));

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_lane, remote_lane));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_lane.error());
		}else {
			helix::SendBuffer send_resp;

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			co_await transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}
	}
}

}}

