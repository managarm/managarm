
#include <string.h>
#include <iostream>

#include "protocols/usb/server.hpp"
#include "usb.pb.h"

namespace protocols::usb {

namespace {
	async::result<void> respondWithError(helix::UniqueDescriptor &conversation, UsbError error) {
		managarm::usb::Errors protoErr = managarm::usb::Errors::OTHER;

		switch (error) {
			using enum UsbError;
			using enum managarm::usb::Errors;

			case stall: protoErr = STALL; break;
			case babble: protoErr = BABBLE; break;
			case timeout: protoErr = TIMEOUT; break;
			case unsupported: protoErr = UNSUPPORTED; break;
			case other: protoErr = OTHER; break;
			default: assert(!"Invalid error in respondWithError");
		}

		managarm::usb::SvrResponse resp;
		resp.set_error(protoErr);

		auto ser = resp.SerializeAsString();

		auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size())
			);

		HEL_CHECK(sendResp.error());
	}
} // namespace anonymous

async::detached serveEndpoint(Endpoint endpoint, helix::UniqueLane lane) {
	while(true) {
		auto [accept, recvReq] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		if(accept.error() == kHelErrEndOfLane)
			co_return;

		HEL_CHECK(accept.error());
		HEL_CHECK(recvReq.error());

		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recvReq.data(), recvReq.length());

		if(req.req_type() == managarm::usb::CntReqType::INTERRUPT_TRANSFER_TO_HOST) {
			// FIXME: Fill in the correct DMA pool.
			arch::dma_buffer buffer{nullptr, static_cast<size_t>(req.length())};
			InterruptTransfer transfer{XferFlags::kXferToHost, buffer};
			transfer.allowShortPackets = req.allow_short();
			transfer.lazyNotification = req.lazy_notification();
			auto outcome = co_await endpoint.transfer(transfer);

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			auto length = outcome.value();

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();

			auto [sendResp, sendData] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::sendBuffer(buffer.data(), length)
				);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendData.error());
		}else if(req.req_type() == managarm::usb::CntReqType::BULK_TRANSFER_TO_DEVICE) {
			// FIXME: Fill in the correct DMA pool.
			arch::dma_buffer buffer{nullptr, static_cast<size_t>(req.length())};

			auto [recvBuffer] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(buffer.data(), buffer.size())
				);

			HEL_CHECK(recvBuffer.error());

			BulkTransfer transfer{XferFlags::kXferToDevice, buffer};
			transfer.lazyNotification = req.lazy_notification();
			auto outcome = co_await endpoint.transfer(transfer);

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			auto length = outcome.value();

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);
			resp.set_size(length);

			auto ser = resp.SerializeAsString();

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);

			HEL_CHECK(sendResp.error());
		}else if(req.req_type() == managarm::usb::CntReqType::BULK_TRANSFER_TO_HOST) {
			// FIXME: Fill in the correct DMA pool.
			arch::dma_buffer buffer{nullptr, static_cast<size_t>(req.length())};
			BulkTransfer transfer{XferFlags::kXferToHost, buffer};
			transfer.allowShortPackets = req.allow_short();
			transfer.lazyNotification = req.lazy_notification();
			auto outcome = co_await endpoint.transfer(transfer);

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			auto length = outcome.value();

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();

			auto [sendResp, sendData] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::sendBuffer(buffer.data(), length)
				);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendData.error());
		}else{
			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);

			HEL_CHECK(sendResp.error());
		}
	}
}

async::detached serveInterface(Interface interface, helix::UniqueLane lane) {
	while(true) {
		auto [accept, recvReq] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		if(accept.error() == kHelErrEndOfLane)
			co_return;

		HEL_CHECK(accept.error());
		HEL_CHECK(recvReq.error());

		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recvReq.data(), recvReq.length());

		if(req.req_type() == managarm::usb::CntReqType::GET_ENDPOINT) {
			auto outcome = co_await interface.getEndpoint(static_cast<PipeType>(req.pipetype()),
					req.number());

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			auto endpoint = std::move(outcome.value());

			helix::UniqueLane localLane, remoteLane;
			std::tie(localLane, remoteLane) = helix::createStream();

			serveEndpoint(std::move(endpoint), std::move(localLane));

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();

			auto [sendResp, sendLane] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::pushDescriptor(remoteLane)
				);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendLane.error());
		}else {
			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);

			HEL_CHECK(sendResp.error());
		}
	}
}

async::detached serveConfiguration(Configuration configuration, helix::UniqueLane lane) {
	while(true) {
		auto [accept, recvReq] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		if(accept.error() == kHelErrEndOfLane)
			co_return;

		HEL_CHECK(accept.error());
		HEL_CHECK(recvReq.error());

		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recvReq.data(), recvReq.length());

		if(req.req_type() == managarm::usb::CntReqType::USE_INTERFACE) {
			auto outcome = co_await configuration.useInterface(req.number(),
					req.alternative());

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			auto interface = std::move(outcome.value());

			helix::UniqueLane localLane, remoteLane;
			std::tie(localLane, remoteLane) = helix::createStream();
			serveInterface(std::move(interface), std::move(localLane));

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();

			auto [sendResp, sendLane] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::pushDescriptor(remoteLane)
				);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendLane.error());
		}else {
			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);

			HEL_CHECK(sendResp.error());
		}
	}
}

async::detached serve(Device device, helix::UniqueLane lane) {
	while(true) {
		auto [accept, recvReq] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		if(accept.error() == kHelErrEndOfLane)
			co_return;

		HEL_CHECK(accept.error());
		HEL_CHECK(recvReq.error());

		auto conversation = accept.descriptor();

		managarm::usb::CntRequest req;
		req.ParseFromArray(recvReq.data(), recvReq.length());

		if(req.req_type() == managarm::usb::CntReqType::GET_CONFIGURATION_DESCRIPTOR) {
			auto outcome = co_await device.configurationDescriptor();

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			auto data = std::move(outcome.value());

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();

			auto [sendResp, sendData] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::sendBuffer(data.data(), data.size())
				);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendData.error());
		}else if(req.req_type() == managarm::usb::CntReqType::TRANSFER_TO_HOST) {
			arch::dma_object<SetupPacket> setup(nullptr);

			auto [recvBuffer] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(setup.data(), sizeof(SetupPacket))
				);

			HEL_CHECK(recvBuffer.error());

			arch::dma_buffer buffer{nullptr, static_cast<size_t>(req.length())};
			auto outcome = co_await device.transfer(ControlTransfer{
					XferFlags::kXferToHost, setup, buffer});

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();

			auto [sendResp, sendData] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::sendBuffer(buffer.data(), buffer.size())
				);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendData.error());
		}else if(req.req_type() == managarm::usb::CntReqType::USE_CONFIGURATION) {
			auto outcome = co_await device.useConfiguration(req.number());

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			auto configuration = std::move(outcome.value());

			helix::UniqueLane localLane, remoteLane;
			std::tie(localLane, remoteLane) = helix::createStream();
			serveConfiguration(std::move(configuration), std::move(localLane));

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();

			auto [sendResp, sendLane] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()),
					helix_ng::pushDescriptor(remoteLane)
				);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendLane.error());
		}else {
			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto ser = resp.SerializeAsString();

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBuffer(ser.data(), ser.size())
				);

			HEL_CHECK(sendResp.error());
		}
	}
}

} // namespace protocols::usb
