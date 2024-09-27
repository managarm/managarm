#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <hw.frigg_bragi.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/acpi/battery.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/mbus.hpp>
#include <thor-internal/main.hpp>
#include <uacpi/notify.h>
#include <uacpi/utilities.h>

namespace thor::acpi {

struct BatteryBusObject final : public KernelBusObject {
	BatteryBusObject(size_t id, uacpi_namespace_node *node)
	: KernelBusObject(), _id{id}, _node{node} {
	}

	coroutine<void> run();

private:
	coroutine<frg::expected<Error>> handleRequest(LaneHandle lane) override;
	static uacpi_status notification(uacpi_handle, uacpi_namespace_node *node, uacpi_u64 value);

	size_t _id;
	uacpi_namespace_node *_node;
	async::recurring_event _irq;
};

} // namespace thor::acpi

namespace {

constexpr bool logBatteryNotifications = true;
constexpr bool logBatteryUpdates = true;

size_t next_battery_id = 0;

}

namespace thor::acpi {

coroutine<void> BatteryBusObject::run() {
	auto obj = frg::construct<AcpiObject>(*kernelAlloc, _node, ACPI_HID_BATTERY, _id);
	co_await obj->run();
	auto acpi_object = obj->mbus_id;

	Properties properties;
	properties.stringProperty("class", frg::string<KernelAlloc>(*kernelAlloc, "power_supply"));
	properties.stringProperty("power_supply.type", frg::string<KernelAlloc>(*kernelAlloc, "battery"));
	properties.stringProperty("power_supply.id", frg::to_allocated_string(*kernelAlloc, _id));
	properties.stringProperty("drvcore.mbus-parent", frg::to_allocated_string(*kernelAlloc, acpi_object));

	// TODO(qookie): Better error handling here.
	(co_await createObject("battery", std::move(properties))).unwrap();

	uacpi_install_notify_handler(_node, BatteryBusObject::notification, this);
}

coroutine<frg::expected<Error>> BatteryBusObject::handleRequest(LaneHandle lane) {
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

uacpi_status BatteryBusObject::notification(uacpi_handle context, uacpi_namespace_node *node, uacpi_u64 value) {
	if(logBatteryNotifications) {
		auto path = uacpi_namespace_node_generate_absolute_path(node);
		infoLogger() << "thor: battery '" << path << "' received AML Notify(" << value << ")" << frg::endlog;
		uacpi_free_absolute_path(path);
	}

	auto self = reinterpret_cast<BatteryBusObject *>(context);
	self->_irq.raise();

	return UACPI_STATUS_OK;
}
void initializeBatteries() {
	async::detach_with_allocator(*kernelAlloc, []() -> coroutine<void> {
		co_await acpiFiber->associatedWorkQueue()->schedule();

		uacpi_find_devices(ACPI_HID_BATTERY, [](void *, uacpi_namespace_node *node) {
			auto bif = uacpi_namespace_node_find(node, "_BIF");
			auto bst = uacpi_namespace_node_find(node, "_BST");

			if(!bif || !bst)
				return UACPI_NS_ITERATION_DECISION_CONTINUE;

			auto obj = frg::construct<BatteryBusObject>(
				*kernelAlloc, next_battery_id++, node);
			async::detach_with_allocator(*kernelAlloc, obj->run());

			return UACPI_NS_ITERATION_DECISION_CONTINUE;
		}, nullptr);
	}());
}

static initgraph::Task initBatteriesTask{&globalInitEngine, "acpi.init-batteries",
	initgraph::Requires{getNsAvailableStage(), getAcpiWorkqueueAvailableStage()},
	[] {
		initializeBatteries();
	}
};

} // namespace thor::acpi
