
#ifdef __x86_64__
#include <arch/io_space.hpp>
#include <thor-internal/arch/hpet.hpp>
#endif
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/acpi/pm-interface.hpp>
#include <thor-internal/mbus.hpp>
#include <hw.frigg_bragi.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>

#include <uacpi/sleep.h>

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

struct PmInterfaceBusObject : private KernelBusObject {
	coroutine<void> run() {
		Properties properties;
		properties.stringProperty("class", frg::string<KernelAlloc>(*kernelAlloc, "pm-interface"));

		// TODO(qookie): Better error handling here.
		(co_await createObject("pm-interface", std::move(properties))).unwrap();
	}

private:
	coroutine<frg::expected<Error>> handleRequest(LaneHandle lane) override {
		auto [acceptError, conversation] = co_await AcceptSender{lane};
		if(acceptError != Error::success)
			co_return acceptError;

		auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
		if(reqError != Error::success)
			co_return reqError;

		auto preamble = bragi::read_preamble(reqBuffer);

		if (preamble.error())
			co_return Error::protocolViolation;

		if(preamble.id() == bragi::message_id<managarm::hw::PmResetRequest>) {
			auto req = bragi::parse_head_only<managarm::hw::PmResetRequest>(reqBuffer, *kernelAlloc);

			if (!req) {
				infoLogger() << "thor: Closing lane due to illegal HW request." << frg::endlog;
				co_return Error::protocolViolation;
			}

			auto ret = uacpi_reboot();
			if(uacpi_unlikely_error(ret))
				infoLogger() << "thor: ACPI reset failed: " << uacpi_status_to_string(ret) << frg::endlog;

#ifdef __x86_64__
			issuePs2Reset();
			infoLogger() << "thor: Reset using PS/2 controller failed" << frg::endlog;
#endif
			panicLogger() << "thor: We do not know how to reset" << frg::endlog;
		}else{
			infoLogger() << "thor: Dismissing conversation due to illegal HW request." << frg::endlog;
			co_await DismissSender{conversation};
		}

		co_return frg::success;
	}
};

void initializePmInterface() {
	// Create a fiber to manage requests to the PM interface mbus object.
	KernelFiber::run([=] {
		auto pmIf = frg::construct<PmInterfaceBusObject>(*kernelAlloc);
		async::detach_with_allocator(*kernelAlloc, pmIf->run());
	});
}

} // namespace thor::acpi
