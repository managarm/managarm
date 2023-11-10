#include <thor-internal/stream.hpp>
#include <thor-internal/mbus.hpp>

#include <mbus.frigg_pb.hpp>

namespace thor {

// TODO: Move this to a header file.
extern frg::manual_box<LaneHandle> mbusClient;

coroutine<frg::expected<Error, size_t>> KernelBusObject::createObject(Properties &&properties) {
	auto [offerError, conversation] = co_await OfferSender{*mbusClient};
	if (offerError != Error::success)
		co_return offerError;

	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);

	for (auto property : properties.properties_) {
		managarm::mbus::Property<KernelAlloc> reqProperty(*kernelAlloc);
		reqProperty.set_name(frg::string<KernelAlloc>(*kernelAlloc, property.name));

		auto &mutable_item = reqProperty.mutable_item().mutable_string_item();
		mutable_item.set_value(std::move(property.value));
		req.add_properties(std::move(reqProperty));
	}

	frg::string<KernelAlloc> ser(*kernelAlloc);
	req.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
	memcpy(reqBuffer.data(), ser.data(), ser.size());
	auto reqError = co_await SendBufferSender{conversation, std::move(reqBuffer)};

	if (reqError != Error::success)
		co_return reqError;

	auto [respError, respBuffer] = co_await RecvBufferSender{conversation};

	if (respError != Error::success)
		co_return respError;

	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(respBuffer.data(), respBuffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	auto [descError, descriptor] = co_await PullDescriptorSender{conversation};

	if (descError != Error::success)
		co_return descError;

	if (!descriptor.is<LaneDescriptor>())
		co_return Error::protocolViolation;

	async::detach_with_allocator(*kernelAlloc, handleMbusComms_(
			descriptor.get<LaneDescriptor>().handle));

	co_return resp.id();
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

	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

	if (req.req_type() != managarm::mbus::SvrReqType::BIND)
		co_return Error::protocolViolation;

	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::string<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
	memcpy(respBuffer.data(), ser.data(), ser.size());
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
