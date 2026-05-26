
#include <string.h>
#include <iostream>
#include <bragi/helpers-std.hpp>

#include <core/dispatch.hpp>

#include "protocols/usb/server.hpp"
#include "usb.bragi.hpp"

namespace protocols::usb {

// Forward declarations for serve functions called from visitor structs.
async::detached serveEndpoint(Device device, Endpoint endpoint, helix::UniqueLane lane);
async::detached serveInterface(Device device, Interface interface, helix::UniqueLane lane);
async::detached serveConfiguration(Device device, Configuration configuration, helix::UniqueLane lane);

namespace {

async::result<void> respondWithError(helix::BorrowedDescriptor conversation, UsbError error) {
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
auto handleXferReq(auto &req, auto &endpoint, auto &&...xferArgs) {
	XferType xfer{
		req.dir() == managarm::usb::XferDirection::TO_HOST
		? kXferToHost
		: kXferToDevice,
		std::forward<decltype(xferArgs)>(xferArgs)...
	};

	xfer.allowShortPackets = req.allow_short_packets();
	xfer.lazyNotification = req.lazy_notification();

	return endpoint.transfer(xfer);
};

struct HandleTransfer {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::usb::TransferRequest &&req, helix::BorrowedDescriptor conversation,
	           bragi::preamble) {
		arch::dma_buffer buffer{device.bufferPool(), static_cast<size_t>(req.length())};

		if (req.dir() == managarm::usb::XferDirection::TO_DEVICE) {
			auto [recvBuffer] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::recvBuffer(buffer.data(), buffer.size())
			);

			HEL_CHECK(recvBuffer.error());
		}

		frg::expected<UsbError, uint64_t> outcome;

		switch (req.type()) {
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
				co_return {};
		}

		if (!outcome) {
			co_await respondWithError(conversation, outcome.error());
			co_return {};
		}

		auto length = outcome.value();

		managarm::usb::SvrResponse resp;
		resp.set_error(managarm::usb::Errors::SUCCESS);

		if (req.dir() == managarm::usb::XferDirection::TO_HOST) {
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
		co_return {};
	}

	Device device;
	Endpoint endpoint;
};

struct HandleGetEndpoint {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::usb::GetEndpointRequest &&req, helix::BorrowedDescriptor conversation,
	           bragi::preamble) {
		auto outcome = co_await interface_.getEndpoint(static_cast<PipeType>(req.type()),
				req.number());

		if (!outcome) {
			co_await respondWithError(conversation, outcome.error());
			co_return {};
		}

		auto endpoint = std::move(outcome.value());

		helix::UniqueLane localLane, remoteLane;
		std::tie(localLane, remoteLane) = helix::createStream();

		serveEndpoint(device, std::move(endpoint), std::move(localLane));

		managarm::usb::SvrResponse resp;
		resp.set_error(managarm::usb::Errors::SUCCESS);

		auto [sendResp, sendLane] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::pushDescriptor(remoteLane)
		);

		HEL_CHECK(sendResp.error());
		HEL_CHECK(sendLane.error());
		co_return {};
	}

	Device device;
	Interface interface_;
};

struct HandleUseInterface {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::usb::UseInterfaceRequest &&req, helix::BorrowedDescriptor conversation,
	           bragi::preamble) {
		auto outcome = co_await configuration.useInterface(req.number(),
				req.alternative());

		if (!outcome) {
			co_await respondWithError(conversation, outcome.error());
			co_return {};
		}

		auto interface = std::move(outcome.value());

		helix::UniqueLane localLane, remoteLane;
		std::tie(localLane, remoteLane) = helix::createStream();
		serveInterface(device, std::move(interface), std::move(localLane));

		managarm::usb::SvrResponse resp;
		resp.set_error(managarm::usb::Errors::SUCCESS);

		auto [sendResp, sendLane] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::pushDescriptor(remoteLane)
		);

		HEL_CHECK(sendResp.error());
		HEL_CHECK(sendLane.error());
		co_return {};
	}

	Device device;
	Configuration configuration;
};

struct HandleServe {
	async::result<std::expected<void, DispatchError>>
	operator()(managarm::usb::GetConfigurationDescriptorRequest &&req,
	           helix::BorrowedDescriptor conversation, bragi::preamble) {
		auto outcome = co_await device.configurationDescriptor(req.configuration());

		if (!outcome) {
			co_await respondWithError(conversation, outcome.error());
			co_return {};
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
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::usb::GetDeviceDescriptorRequest &&,
	           helix::BorrowedDescriptor conversation, bragi::preamble) {
		auto outcome = co_await device.deviceDescriptor();

		if (!outcome) {
			co_await respondWithError(conversation, outcome.error());
			co_return {};
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
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::usb::TransferRequest &&req,
	           helix::BorrowedDescriptor conversation, bragi::preamble) {
		if (req.type() != managarm::usb::XferType::CONTROL) {
			co_await respondWithError(conversation, UsbError::unsupported);
			co_return {};
		}

		arch::dma_object<SetupPacket> setup{device.setupPool()};

		auto [recvBuffer] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::recvBuffer(setup.data(), sizeof(SetupPacket))
		);

		HEL_CHECK(recvBuffer.error());

		arch::dma_buffer buffer{device.bufferPool(), static_cast<size_t>(req.length())};

		if (req.dir() == managarm::usb::XferDirection::TO_DEVICE) {
			auto [recvData] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::recvBuffer(buffer.data(), buffer.size())
			);

			HEL_CHECK(recvData.error());
		}

		ControlTransfer xfer{
			req.dir() == managarm::usb::XferDirection::TO_HOST
			? kXferToHost
			: kXferToDevice,
			setup, buffer
		};

		auto outcome = co_await device.transfer(xfer);

		if (!outcome) {
			co_await respondWithError(conversation, outcome.error());
			co_return {};
		}

		managarm::usb::SvrResponse resp;
		resp.set_error(managarm::usb::Errors::SUCCESS);

		if (req.dir() == managarm::usb::XferDirection::TO_HOST) {
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
		co_return {};
	}

	async::result<std::expected<void, DispatchError>>
	operator()(managarm::usb::UseConfigurationRequest &&req,
	           helix::BorrowedDescriptor conversation, bragi::preamble) {
		auto outcome = co_await device.useConfiguration(req.index(), req.value());

		if (!outcome) {
			co_await respondWithError(conversation, outcome.error());
			co_return {};
		}

		auto configuration = std::move(outcome.value());

		helix::UniqueLane localLane, remoteLane;
		std::tie(localLane, remoteLane) = helix::createStream();
		serveConfiguration(device, std::move(configuration), std::move(localLane));

		managarm::usb::SvrResponse resp;
		resp.set_error(managarm::usb::Errors::SUCCESS);

		auto [sendResp, sendLane] = co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::pushDescriptor(remoteLane)
		);

		HEL_CHECK(sendResp.error());
		HEL_CHECK(sendLane.error());
		co_return {};
	}

	Device device;
};

} // namespace anonymous

async::detached serveEndpoint(Device device, Endpoint endpoint, helix::UniqueLane lane) {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::usb::TransferRequest
		>(lane, HandleTransfer{device, endpoint});
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "usb: dispatch error on endpoint lane" << std::endl;
			continue;
		}
	}
}

async::detached serveInterface(Device device, Interface interface, helix::UniqueLane lane) {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::usb::GetEndpointRequest
		>(lane, HandleGetEndpoint{device, interface});
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "usb: dispatch error on interface lane" << std::endl;
			continue;
		}
	}
}

async::detached serveConfiguration(Device device, Configuration configuration, helix::UniqueLane lane) {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::usb::UseInterfaceRequest
		>(lane, HandleUseInterface{device, configuration});
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "usb: dispatch error on configuration lane" << std::endl;
			continue;
		}
	}
}

async::detached serve(Device device, helix::UniqueLane lane) {
	while(true) {
		auto res = co_await dispatchRequest<
			managarm::usb::GetConfigurationDescriptorRequest,
			managarm::usb::GetDeviceDescriptorRequest,
			managarm::usb::TransferRequest,
			managarm::usb::UseConfigurationRequest
		>(lane, HandleServe{device});
		if(!res) {
			if(res.error() == DispatchError::shutdown)
				co_return;
			std::cout << "usb: dispatch error on device lane" << std::endl;
			continue;
		}
	}
}

} // namespace protocols::usb
