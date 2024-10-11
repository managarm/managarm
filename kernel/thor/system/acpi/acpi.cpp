#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/acpi/battery.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <uacpi/resources.h>

namespace thor::acpi {

coroutine<void> AcpiObject::run() {
	auto path = uacpi_namespace_node_generate_absolute_path(node);

	Properties acpi_properties;
	acpi_properties.stringProperty("unix.subsystem", frg::string<KernelAlloc>(*kernelAlloc, "acpi"));
	acpi_properties.stringProperty("acpi.path", frg::string<KernelAlloc>(path, *kernelAlloc));
	if(hid_name)
		acpi_properties.stringProperty("acpi.hid", frg::string<KernelAlloc>(*kernelAlloc, hid_name->value));
	if(cid_name && cid_name->num_ids)
		acpi_properties.stringProperty("acpi.cid", frg::string<KernelAlloc>(*kernelAlloc, cid_name->ids[0].value));
	acpi_properties.stringProperty("acpi.instance", frg::to_allocated_string(*kernelAlloc, instance));

	uacpi_free_absolute_path(path);

	mbus_id = (co_await createObject("acpi-object", std::move(acpi_properties))).unwrap();

	completion.raise();
}

coroutine<frg::expected<Error>> AcpiObject::handleRequest(LaneHandle lane) {
	auto [acceptError, conversation] = co_await AcceptSender{lane};
	if(acceptError != Error::success)
		co_return acceptError;

	auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
	if(reqError != Error::success)
		co_return reqError;

	auto preamble = bragi::read_preamble(reqBuffer);

	if(preamble.error())
		co_return Error::protocolViolation;

	infoLogger() << "thor: dismissing conversation due to illegal HW request." << frg::endlog;
	co_await DismissSender{conversation};

	co_return frg::success;
}

initgraph::Stage *getAcpiWorkqueueAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "acpi.workqueue-available"};
	return &s;
}

static initgraph::Task initAcpiWorkqueueTask{&globalInitEngine, "acpi.init-acpi-workqueue",
	initgraph::Requires{getFibersAvailableStage()},
	initgraph::Entails{getAcpiWorkqueueAvailableStage()},
	[] {
		// Create a fiber to manage requests to the battery mbus objects.
		acpiFiber = KernelFiber::post([] {
			// Do nothing. Our only purpose is to run the associated work queue.
		});

		Scheduler::resume(acpiFiber);
	}
};

} // namespace thor::acpi
