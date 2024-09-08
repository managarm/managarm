
#include <string.h>
#include <iostream>
#include <bragi/helpers-std.hpp>

#include "protocols/usb/server.hpp"
#include "usb.bragi.hpp"

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

	auto [sendResp] = co_await helix_ng::exchangeMsgs(
		conversation,
		helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
	);

	HEL_CHECK(sendResp.error());
}

template <typename XferType>
auto handleXferReq(auto req, auto &endpoint, auto &&...xferArgs) {
	XferType xfer{
		req->dir() == managarm::usb::XferDirection::TO_HOST
		? kXferToHost
		: kXferToDevice,
		std::forward<decltype(xferArgs)>(xferArgs)...
	};

	if (req->dir() == managarm::usb::XferDirection::TO_DEVICE)
		xfer.allowShortPackets = req->allow_short_packets();
	xfer.lazyNotification = req->lazy_notification();

	return endpoint.transfer(xfer);
};

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

		auto preamble = bragi::read_preamble(recvReq);
		if (preamble.error())
			co_return;

		if (preamble.id() == bragi::message_id<managarm::usb::TransferRequest>) {
			auto req = bragi::parse_head_only<managarm::usb::TransferRequest>(recvReq);
			if (!req) {
				co_return;
			}

			// TODO(qookie): Use proper pool:
			//		 something like ep.device.bufferPool()
			arch::dma_buffer buffer{nullptr, static_cast<size_t>(req->length())};

			if (req->dir() == managarm::usb::XferDirection::TO_DEVICE) {
				auto [recvBuffer] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(buffer.data(), buffer.size())
				);

				HEL_CHECK(recvBuffer.error());
			}

			frg::expected<UsbError, uint64_t> outcome;

			switch (req->type()) {
				using enum managarm::usb::XferType;
				case INTERRUPT:
					outcome = co_await handleXferReq<InterruptTransfer>(req, endpoint, buffer);
					break;
				case BULK:
					outcome = co_await handleXferReq<BulkTransfer>(req, endpoint, buffer);
					break;
					// TODO(qookie): Support control EPs
					//case CONTROL:
					//	outcome = co_await handleXferReq<ControlTransfer>(req, endpoint, buffer);
					//	break;
				default:
					std::cout << "Unexpected endpoint type\n";
					co_return;
			}

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			auto length = outcome.value();

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			if (req->dir() == managarm::usb::XferDirection::TO_HOST) {
				auto [sendResp, sendData] =
					co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
						helix_ng::sendBuffer(buffer.data(), length)
					);

				HEL_CHECK(sendResp.error());
				HEL_CHECK(sendData.error());
			} else {
				auto [sendResp] =
					co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
					);

				HEL_CHECK(sendResp.error());
			}
		}else{
			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
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

		auto preamble = bragi::read_preamble(recvReq);
		if (preamble.error())
			co_return;

		if (preamble.id() == bragi::message_id<managarm::usb::GetEndpointRequest>) {
			auto req = bragi::parse_head_only<managarm::usb::GetEndpointRequest>(recvReq);
			if (!req) {
				co_return;
			}

			auto outcome = co_await interface.getEndpoint(static_cast<PipeType>(req->type()),
					req->number());

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

			auto [sendResp, sendLane] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::pushDescriptor(remoteLane)
			);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendLane.error());
		}else {
			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
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

		auto preamble = bragi::read_preamble(recvReq);
		if (preamble.error())
			co_return;

		if (preamble.id() == bragi::message_id<managarm::usb::UseInterfaceRequest>) {
			auto req = bragi::parse_head_only<managarm::usb::UseInterfaceRequest>(recvReq);
			if (!req) {
				co_return;
			}

			auto outcome = co_await configuration.useInterface(req->number(),
					req->alternative());

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

			auto [sendResp, sendLane] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::pushDescriptor(remoteLane)
			);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendLane.error());
		}else {
			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
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

		auto preamble = bragi::read_preamble(recvReq);
		if (preamble.error())
			co_return;

		if (preamble.id() == bragi::message_id<managarm::usb::GetConfigurationDescriptorRequest>) {
			auto req = bragi::parse_head_only<managarm::usb::GetConfigurationDescriptorRequest>(recvReq);
			if (!req) {
				co_return;
			}

			auto outcome = co_await device.configurationDescriptor(req->configuration());

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			auto data = std::move(outcome.value());

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);
			resp.set_size(data.size());

			auto [sendResp, sendData] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(data.data(), data.size())
			);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendData.error());
		} else if (preamble.id() == bragi::message_id<managarm::usb::GetDeviceDescriptorRequest>) {
			auto req = bragi::parse_head_only<managarm::usb::GetDeviceDescriptorRequest>(recvReq);
			if (!req) {
				co_return;
			}

			auto outcome = co_await device.deviceDescriptor();

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			auto data = std::move(outcome.value());

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			auto [sendResp, sendData] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::sendBuffer(data.data(), data.size())
			);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendData.error());
		} else if (preamble.id() == bragi::message_id<managarm::usb::TransferRequest>) {
			auto req = bragi::parse_head_only<managarm::usb::TransferRequest>(recvReq);
			if (!req) {
				co_return;
			}

			if (req->type() != managarm::usb::XferType::CONTROL) {
				co_await respondWithError(conversation, UsbError::unsupported);
				continue;
			}

			arch::dma_object<SetupPacket> setup{nullptr};

			auto [recvBuffer] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::recvBuffer(setup.data(), sizeof(SetupPacket))
			);

			HEL_CHECK(recvBuffer.error());

			arch::dma_buffer buffer{nullptr, static_cast<size_t>(req->length())};

			if (req->dir() == managarm::usb::XferDirection::TO_DEVICE) {
				auto [recvBuffer] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(buffer.data(), buffer.size())
				);

				HEL_CHECK(recvBuffer.error());
			}

			ControlTransfer xfer{
				req->dir() == managarm::usb::XferDirection::TO_HOST
				? kXferToHost
				: kXferToDevice,
				setup, buffer
			};

			auto outcome = co_await device.transfer(xfer);

			if (!outcome) {
				co_await respondWithError(conversation, outcome.error());
				continue;
			}

			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::SUCCESS);

			if (req->dir() == managarm::usb::XferDirection::TO_HOST) {
				resp.set_size(outcome.value());

				auto [sendResp, sendData] =
					co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
						helix_ng::sendBuffer(buffer.data(), buffer.size())
					);

				HEL_CHECK(sendResp.error());
				HEL_CHECK(sendData.error());
			} else {
				auto [sendResp] =
					co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
					);

				HEL_CHECK(sendResp.error());
			}
		} else if (preamble.id() == bragi::message_id<managarm::usb::UseConfigurationRequest>) {
			auto req = bragi::parse_head_only<managarm::usb::UseConfigurationRequest>(recvReq);
			if (!req) {
				co_return;
			}

			auto outcome = co_await device.useConfiguration(req->index(), req->value());

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

			auto [sendResp, sendLane] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
				helix_ng::pushDescriptor(remoteLane)
			);

			HEL_CHECK(sendResp.error());
			HEL_CHECK(sendLane.error());
		}else {
			managarm::usb::SvrResponse resp;
			resp.set_error(managarm::usb::Errors::ILLEGAL_REQUEST);

			auto [sendResp] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
			);

			HEL_CHECK(sendResp.error());
		}
	}
}

} // namespace protocols::usb
