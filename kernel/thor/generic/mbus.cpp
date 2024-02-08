#include <thor-internal/stream.hpp>
#include <thor-internal/mbus.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>

#include <mbus.frigg_bragi.hpp>

namespace thor {

// TODO: Move this to a header file.
extern frg::manual_box<LaneHandle> mbusClient;

coroutine<frg::expected<Error, size_t>> KernelBusObject::createObject(frg::string_view name, Properties &&properties) {
	auto [offerError, conversation] = co_await OfferSender{*mbusClient};
	if (offerError != Error::success)
		co_return offerError;

	managarm::mbus::CreateObjectRequest<KernelAlloc> req(*kernelAlloc);
	req.set_name(frg::string<KernelAlloc>{name, *kernelAlloc});

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

	auto [descError, descriptor] = co_await PullDescriptorSender{conversation};

	if (descError != Error::success)
		co_return descError;
	if (!descriptor.is<LaneDescriptor>())
		co_return Error::protocolViolation;

	auto resp = bragi::parse_head_only<managarm::mbus::CreateObjectResponse>(respBuffer, *kernelAlloc);
	if (!resp)
		co_return Error::protocolViolation;
	if (resp->error() != managarm::mbus::Error::SUCCESS)
		co_return Error::illegalState;

	async::detach_with_allocator(*kernelAlloc,
			handleMbusComms_(descriptor.get<LaneDescriptor>().handle));

	co_return resp->id();
}

coroutine<void> KernelBusObject::handleMbusComms_(LaneHandle mgmtLane) {
	while (true) {
		// TODO(qookie): Improve error handling here?
		// Perhaps we should at least log a message.
		(void)(co_await handleServeRemoteLane_(mgmtLane));
	}
}

coroutine<frg::expected<Error>> KernelBusObject::handleServeRemoteLane_(LaneHandle mgmtLane) {
	auto [offerError, conversation] = co_await OfferSender{mgmtLane};
	if (offerError != Error::success)
		co_return offerError;

	managarm::mbus::ServeRemoteLaneRequest<KernelAlloc> req(*kernelAlloc);

	frg::unique_memory<KernelAlloc> headBuffer{*kernelAlloc, req.size_of_head()};
	bragi::write_head_only(req, headBuffer);
	auto headError = co_await SendBufferSender{conversation, std::move(headBuffer)};

	if (headError != Error::success)
		co_return headError;

	auto lane = initiateClient();

	auto descError = co_await PushDescriptorSender{conversation,
		LaneDescriptor{lane}};

	if (descError != Error::success)
		co_return descError;

	auto [respError, respBuffer] = co_await RecvBufferSender{conversation};

	if (respError != Error::success)
		co_return respError;

	auto resp = bragi::parse_head_only<managarm::mbus::ServeRemoteLaneResponse>(respBuffer, *kernelAlloc);
	if (!resp)
		co_return Error::protocolViolation;
	if (resp->error() != managarm::mbus::Error::SUCCESS)
		co_return Error::illegalState;

	co_return frg::success;
}

} // namespace thor
