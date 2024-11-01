#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <uacpi/utilities.h>

namespace {

size_t next_keyboard_id = 0;
size_t next_mouse_id = 0;

struct AcpiStatus final : public thor::KernelBusObject {
	AcpiStatus(const char *status)
	    : status_{frg::string<thor::KernelAlloc>(*thor::kernelAlloc, status)} {}

	coroutine<void> run() {
		thor::Properties props;
		props.stringProperty(
		    "unix.subsystem", frg::string<thor::KernelAlloc>(*thor::kernelAlloc, "acpi")
		);
		props.stringProperty(
		    "acpi.status", frg::string<thor::KernelAlloc>("ps2.init_complete", *thor::kernelAlloc)
		);

		(co_await createObject("acpi-status", std::move(props))).unwrap();
	}

	coroutine<frg::expected<thor::Error>> handleRequest(thor::LaneHandle lane) override {
		auto [acceptError, conversation] = co_await thor::AcceptSender{lane};
		if (acceptError != thor::Error::success)
			co_return acceptError;

		auto [reqError, reqBuffer] = co_await thor::RecvBufferSender{conversation};
		if (reqError != thor::Error::success)
			co_return reqError;

		auto preamble = bragi::read_preamble(reqBuffer);

		if (preamble.error())
			co_return thor::Error::protocolViolation;

		thor::infoLogger() << "thor: dismissing conversation due to illegal HW request."
		                   << frg::endlog;
		co_await thor::DismissSender{conversation};

		co_return frg::success;
	};

  private:
	frg::string<thor::KernelAlloc> status_;
};

} // namespace

namespace thor::acpi {

void initializePS2() {
	// Create a fiber to manage requests to the ACPI mbus objects.
	async::detach_with_allocator(*kernelAlloc, []() -> coroutine<void> {
		co_await acpiFiber->associatedWorkQueue()->schedule();

		frg::vector<async::oneshot_event *, thor::KernelAlloc> events{*thor::kernelAlloc};

		uacpi_find_devices_at(
		    uacpi_namespace_root(),
		    ACPI_HID_PS2_KEYBOARDS.data(),
		    [](void *ctx, uacpi_namespace_node *node) {
			    auto events =
			        reinterpret_cast<frg::vector<async::oneshot_event *, KernelAlloc> *>(ctx);

			    auto obj = frg::construct<AcpiObject>(*kernelAlloc, node, next_keyboard_id++);
			    events->push_back(&obj->completion);
			    async::detach_with_allocator(*kernelAlloc, obj->run());

			    return UACPI_NS_ITERATION_DECISION_CONTINUE;
		    },
		    &events
		);

		uacpi_find_devices_at(
		    uacpi_namespace_root(),
		    ACPI_HID_PS2_MICE.data(),
		    [](void *ctx, uacpi_namespace_node *node) {
			    auto events =
			        reinterpret_cast<frg::vector<async::oneshot_event *, KernelAlloc> *>(ctx);

			    auto obj = frg::construct<AcpiObject>(*kernelAlloc, node, next_mouse_id++);
			    events->push_back(&obj->completion);
			    async::detach_with_allocator(*kernelAlloc, obj->run());

			    return UACPI_NS_ITERATION_DECISION_CONTINUE;
		    },
		    &events
		);

		for (auto ev : events) {
			co_await ev->wait();
		}

		// This object is published to notify listeners about the fact that ACPI parsing
		// and publishing of PS/2 objects from there has finished, so as to avoid running mbus
		// filters indefinitely.
		AcpiStatus status{"ps2.init-complete"};
		co_await status.run();
	}());
}

static initgraph::Task initPS2Task{
    &globalInitEngine,
    "acpi.init-ps2",
    initgraph::Requires{getNsAvailableStage(), getAcpiWorkqueueAvailableStage()},
    [] { initializePS2(); }
};

} // namespace thor::acpi
