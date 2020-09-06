
#ifdef __x86_64__
#include <arch/io_space.hpp>
#include <thor-internal/arch/hpet.hpp>
#endif
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/acpi/pm-interface.hpp>
#include <hw.frigg_pb.hpp>
#include <mbus.frigg_pb.hpp>

#include <lai/helpers/pm.h>

namespace thor {
	// TODO: Move this to a header file.
	extern frg::manual_box<LaneHandle> mbusClient;
}

namespace thor::acpi {

#ifdef __x86_64__
inline constexpr arch::scalar_register<uint8_t> ps2Command(0x64);

constexpr uint8_t ps2Reset = 0xFE;

void issuePs2Reset() {
	arch::io_space space;
	space.store(ps2Command, ps2Reset);
	pollSleepNano(100'000'000); // 100ms should be long enough to actually reset.
}
#endif

namespace {

coroutine<bool> handleReq(LaneHandle lane) {
	auto [acceptError, conversation] = co_await AcceptSender{lane};
	if(acceptError == Error::endOfLane)
		co_return false;
	// TODO: improve error handling here.
	assert(acceptError == Error::success);

	auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
	// TODO: improve error handling here.
	assert(reqError == Error::success);

	managarm::hw::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

	if(req.req_type() == managarm::hw::CntReqType::PM_RESET) {
		if(lai_acpi_reset())
			infoLogger() << "thor: ACPI reset failed" << frg::endlog;

#ifdef __x86_64__
		issuePs2Reset();
		infoLogger() << "thor: Reset using PS/2 controller failed" << frg::endlog;
#endif
		panicLogger() << "thor: We do not know how to reset" << frg::endlog;
	}else{
		managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::hw::Errors::ILLEGAL_REQUEST);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
		// TODO: improve error handling here.
		assert(respError == Error::success);
	}

	co_return true;
}

// ------------------------------------------------------------------------
// mbus object creation and management.
// ------------------------------------------------------------------------

coroutine<LaneHandle> createObject(LaneHandle mbusLane) {
	auto [offerError, conversation] = co_await OfferSender{mbusLane};
	// TODO: improve error handling here.
	assert(offerError == Error::success);

	managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
	cls_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "class"));
	auto &cls_item = cls_prop.mutable_item().mutable_string_item();
	cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "pm-interface"));

	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(cls_prop));

	frg::string<KernelAlloc> ser(*kernelAlloc);
	req.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
	memcpy(reqBuffer.data(), ser.data(), ser.size());
	auto reqError = co_await SendBufferSender{conversation, std::move(reqBuffer)};
	// TODO: improve error handling here.
	assert(reqError == Error::success);

	auto [respError, respBuffer] = co_await RecvBufferSender{conversation};
	// TODO: improve error handling here.
	assert(respError == Error::success);
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(respBuffer.data(), respBuffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	auto [descError, descriptor] = co_await PullDescriptorSender{conversation};
	// TODO: improve error handling here.
	assert(descError == Error::success);
	assert(descriptor.is<LaneDescriptor>());
	co_return descriptor.get<LaneDescriptor>().handle;
}

coroutine<void> handleBind(LaneHandle objectLane) {
	auto [acceptError, conversation] = co_await AcceptSender{objectLane};
	// TODO: improve error handling here.
	assert(acceptError == Error::success);

	auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
	// TODO: improve error handling here.
	assert(reqError == Error::success);
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);

	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::string<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
	memcpy(respBuffer.data(), ser.data(), ser.size());
	auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
	// TODO: improve error handling here.
	assert(respError == Error::success);

	auto stream = createStream();
	auto descError = co_await PushDescriptorSender{conversation,
			LaneDescriptor{stream.get<1>()}};
	// TODO: improve error handling here.
	assert(descError == Error::success);

	async::detach_with_allocator(*kernelAlloc, [] (LaneHandle lane) -> coroutine<void> {
		while(true) {
			if(!(co_await handleReq(lane)))
				break;
		}
	}(std::move(stream.get<0>())));
}

} // anonymous namespace

void initializePmInterface() {
	// Create a fiber to manage requests to the RTC mbus object.
	KernelFiber::run([=] {
		async::detach_with_allocator(*kernelAlloc, [] () -> coroutine<void> {
			auto objectLane = co_await createObject(*mbusClient);
			while(true)
				co_await handleBind(objectLane);
		}());
	});
}

} // namespace thor::acpi
