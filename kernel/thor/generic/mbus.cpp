#include <thor-internal/stream.hpp>
#include <thor-internal/mbus.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>

#include <mbus.frigg_bragi.hpp>

namespace thor {

// TODO: Move this to a header file.
extern frg::manual_box<LaneHandle> mbusClient;

coroutine<frg::expected<Error, size_t>> KernelBusObject::createObject(Properties &&properties) {
	auto [offerError, conversation] = co_await OfferSender{*mbusClient};
	if (offerError != Error::success)
		co_return offerError;

	managarm::mbus::CreateObjectRequest<KernelAlloc> req(*kernelAlloc);
	req.set_parent_id(1);

	for (auto property : properties.properties_) {
		managarm::mbus::Property<KernelAlloc> reqProperty(*kernelAlloc);
		reqProperty.set_name(frg::string<KernelAlloc>(*kernelAlloc, property.name));
		reqProperty.set_string_item(std::move(property.value));
		req.add_properties(std::move(reqProperty));
	}


	frg::unique_memory<KernelAlloc> headBuffer{*kernelAlloc, req.size_of_head()};
	frg::unique_memory<KernelAlloc> tailBuffer{*kernelAlloc, req.size_of_tail()};
	bragi::write_head_tail(req, headBuffer, tailBuffer);
	auto headError = co_await SendBufferSender{conversation, std::move(headBuffer)};
	auto tailError = co_await SendBufferSender{conversation, std::move(tailBuffer)};

	if (headError != Error::success)
		co_return headError;
	if (tailError != Error::success)
		co_return tailError;

	auto [respError, respBuffer] = co_await RecvBufferSender{conversation};

	if (respError != Error::success)
		co_return respError;

	auto resp = bragi::parse_head_only<managarm::mbus::SvrResponse>(respBuffer, *kernelAlloc);
	if (!resp)
		co_return Error::protocolViolation;
	if (resp->error() != managarm::mbus::Error::SUCCESS)
		co_return Error::illegalState;

	auto [descError, descriptor] = co_await PullDescriptorSender{conversation};

	if (descError != Error::success)
		co_return descError;

	if (!descriptor.is<LaneDescriptor>())
		co_return Error::protocolViolation;

	async::detach_with_allocator(*kernelAlloc, handleMbusComms_(
			descriptor.get<LaneDescriptor>().handle));

	co_return resp->id();
}

coroutine<void> KernelBusObject::handleMbusComms_(LaneHandle objectLane) {
	while (true) {
		// TODO(qookie): Improve error handling here?
		// Perhaps we should at least log a message.
		(void)(co_await handleBind_(objectLane));
	}
}

coroutine<frg::expected<Error>> KernelBusObject::handleBind_(LaneHandle objectLane) {
	auto [acceptError, conversation] = co_await AcceptSender{objectLane};

	if (acceptError != Error::success)
		co_return acceptError;

	auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};

	if (reqError != Error::success)
		co_return reqError;

	auto req = bragi::parse_head_only<managarm::mbus::S2CBindRequest>(reqBuffer, *kernelAlloc);
	if (!req)
		co_return Error::protocolViolation;

	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.size_of_head()};
	bragi::write_head_only(resp, respBuffer);
	auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};

	if (respError != Error::success)
		co_return respError;

	auto lane = initiateClient();

	auto descError = co_await PushDescriptorSender{conversation,
		LaneDescriptor{lane}};

	if (descError != Error::success)
		co_return descError;

	co_return frg::success;
}

} // namespace thor
