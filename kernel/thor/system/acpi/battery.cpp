#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/utility.hpp>
#include <hw.frigg_bragi.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/acpi/battery.hpp>
#include <thor-internal/acpi/helpers.hpp>
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

	void updateBIF();
	void updateBST();
	void updateState();

	struct BatteryState {
		// the units in which ACPI reports its data
		frg::optional<int> acpi_units = frg::null_opt;
		frg::optional<int> battery_technology = frg::null_opt;

		bool charging;
		// the rate of (dis)charge in mW
		frg::optional<uint32_t> rate_milliwatt;
		// the rate of (dis)charge in mA
		frg::optional<uint32_t> rate_milliampere;
		// the voltage across the battery terminals
		frg::optional<uint32_t> voltage;
		frg::optional<uint32_t> design_voltage;

		frg::optional<uint32_t> remaining_capacity_milliwatthours;
		frg::optional<uint32_t> remaining_capacity_milliamperehours;
		frg::optional<uint32_t> design_capacity_milliwatthours;
		frg::optional<uint32_t> design_capacity_milliamperehours;
		frg::optional<uint32_t> last_full_charge_capacity_milliwatthours;
		frg::optional<uint32_t> last_full_charge_capacity_milliamperehours;
	};

	size_t _id;
	uacpi_namespace_node *_node;
	async::recurring_event _irq;
	BatteryState _state = {};
};

} // namespace thor::acpi

namespace {

constexpr bool logBatteryNotifications = true;
constexpr bool logBatteryUpdates = true;

size_t next_battery_id = 0;

namespace BIF {
	namespace PowerUnit {
		constexpr uacpi_u32 MILLIWATT = 0;
		constexpr uacpi_u32 MILLIAMPERE = 1;
	} // namespace PowerUnit

	namespace DesignCapacity {
		constexpr uacpi_u32 UNKNOWN = 0xFFFFFFFF;
	} // namespace DesignCapacity

	namespace LastFullChargeCapacity {
		constexpr uacpi_u32 UNKNOWN = 0xFFFFFFFF;
	} // namespace LastFullChargeCapacity

	namespace BatteryTechnology {
		constexpr uacpi_u32 PRIMARY = 0;
		constexpr uacpi_u32 SECONDARY = 1;
	} // namespace BatteryTechnology

	namespace DesignVoltage {
		constexpr uacpi_u32 UNKNOWN = 0xFFFFFFFF;
	} // namespace DesignVoltage
} // namespace BIF

namespace BST {
	namespace State {
		constexpr uacpi_u32 DISCHARGING = (1 << 0);
		constexpr uacpi_u32 CHARGING = (1 << 1);
		constexpr uacpi_u32 CRITICAL_ENERGY_STATE = (1 << 2);
		constexpr uacpi_u32 CHARGE_LIMITING = (1 << 3);
	} // namespace State

	namespace Rate {
		constexpr uacpi_u32 UNKNOWN = 0xFFFFFFFF;
	} // namespace Rate

	namespace Voltage {
		constexpr uacpi_u32 UNKNOWN = 0xFFFFFFFF;
	} // namespace Voltage

	namespace Capacity {
		constexpr uacpi_u32 UNKNOWN = 0xFFFFFFFF;
	} // namespace Capacity
} // namespace BST

} // namespace

namespace thor::acpi {

coroutine<void> BatteryBusObject::run() {
	auto obj = frg::construct<AcpiObject>(*kernelAlloc, _node, _id);
	co_await obj->run();
	auto acpi_object = obj->mbus_id;

	updateState();

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

	if(preamble.id() == bragi::message_id<managarm::hw::BatteryStateRequest>) {
		auto req = bragi::parse_head_only<managarm::hw::BatteryStateRequest>(reqBuffer, *kernelAlloc);

		if(req->block_until_ready())
			co_await _irq.async_wait();

		managarm::hw::BatteryStateReply<KernelAlloc> resp(*kernelAlloc);

		resp.set_charging(_state.charging);
		if(_state.rate_milliampere)
			resp.set_current_now(*_state.rate_milliampere * 1000);
		if(_state.rate_milliwatt)
			resp.set_power_now(*_state.rate_milliwatt * 1000);
		if(_state.remaining_capacity_milliwatthours)
			resp.set_energy_now(*_state.remaining_capacity_milliwatthours * 1000);
		if(_state.last_full_charge_capacity_milliwatthours)
			resp.set_energy_full(*_state.last_full_charge_capacity_milliwatthours * 1000);
		if(_state.design_capacity_milliwatthours)
			resp.set_energy_full_design(*_state.design_capacity_milliwatthours * 1000);
		if(_state.voltage)
			resp.set_voltage_now(*_state.voltage * 1000);
		if(_state.design_voltage)
			resp.set_voltage_min_design(*_state.design_voltage * 1000);

		resp.set_error(managarm::hw::Errors::SUCCESS);

		frg::unique_memory<KernelAlloc> respHeadBuffer{*kernelAlloc, resp.head_size};
		frg::unique_memory<KernelAlloc> respTailBuffer{*kernelAlloc, resp.size_of_tail()};

		bragi::write_head_tail(resp, respHeadBuffer, respTailBuffer);

		auto respHeadError = co_await SendBufferSender{conversation, std::move(respHeadBuffer)};
		if(respHeadError != Error::success)
			co_return respHeadError;

		auto respTailError = co_await SendBufferSender{conversation, std::move(respTailBuffer)};
		if(respTailError != Error::success)
			co_return respTailError;

		co_return frg::success;
	}else{
		infoLogger() << "thor: dismissing conversation due to illegal HW request." << frg::endlog;
		co_await DismissSender{conversation};
	}

	co_return frg::success;
}

uacpi_status BatteryBusObject::notification(uacpi_handle context, uacpi_namespace_node *node, uacpi_u64 value) {
	if(logBatteryNotifications) {
		auto path = uacpi_namespace_node_generate_absolute_path(node);
		infoLogger() << "thor: battery '" << path << "' received AML Notify(" << value << ")" << frg::endlog;
		uacpi_free_absolute_path(path);
	}

	auto self = reinterpret_cast<BatteryBusObject *>(context);

	self->updateState();
	self->_irq.raise();

	return UACPI_STATUS_OK;
}

void BatteryBusObject::updateBIF() {
	uacpi_object *bif = UACPI_NULL;

	auto ret = uacpi_eval_simple_package(_node, "_BIF", &bif);
	if(ret != UACPI_STATUS_OK) {
		infoLogger() << "thor: _BIF error " << uacpi_status_to_string(ret) << frg::endlog;
		return;
	}

	uacpi_object_array pkg;
	ret = uacpi_object_get_package(bif, &pkg);
	if(ret != UACPI_STATUS_OK) {
		infoLogger() << "thor: uacpi_object_get_package(bif) error " << uacpi_status_to_string(ret) << frg::endlog;
		return;
	}

	auto power_unit = intFromPackage(pkg, 0);
	auto design_capacity = intFromPackage(pkg, 1);
	auto last_full_charge_capacity = intFromPackage(pkg, 2);
	auto battery_technology = intFromPackage(pkg, 3);
	auto design_voltage = intFromPackage(pkg, 4);

	if(!power_unit) {
		infoLogger() << "thor: battery power unit: invalid" << frg::endlog;
	} else {
		switch(*power_unit) {
			case BIF::PowerUnit::MILLIWATT:
				_state.acpi_units = BIF::PowerUnit::MILLIWATT;
				break;
			case BIF::PowerUnit::MILLIAMPERE:
				_state.acpi_units = BIF::PowerUnit::MILLIAMPERE;
				break;
			default:
				_state.acpi_units = frg::null_opt;
				break;
		}
	}

	if(_state.acpi_units && design_capacity && *design_capacity != BIF::DesignCapacity::UNKNOWN) {
		if(*_state.acpi_units == BIF::PowerUnit::MILLIAMPERE) {
			_state.design_capacity_milliamperehours = *design_capacity;

			if(_state.voltage)
				_state.design_capacity_milliwatthours = (*_state.design_capacity_milliamperehours) * (*_state.voltage);
		} else if(*_state.acpi_units == BIF::PowerUnit::MILLIWATT) {
			_state.design_capacity_milliwatthours = *design_capacity;

			if(_state.voltage)
				_state.design_capacity_milliamperehours = (*_state.design_capacity_milliwatthours) / (*_state.voltage);
		} else {
			panicLogger() << "thor: unhandled ACPI battery power unit " << *_state.acpi_units << frg::endlog;
		}
	} else {
		_state.design_capacity_milliwatthours = frg::null_opt;
		_state.design_capacity_milliamperehours = frg::null_opt;
	}

	if(_state.acpi_units && last_full_charge_capacity && *last_full_charge_capacity != BIF::LastFullChargeCapacity::UNKNOWN) {
		if(*_state.acpi_units == BIF::PowerUnit::MILLIAMPERE) {
			_state.last_full_charge_capacity_milliamperehours = *last_full_charge_capacity;

			if(_state.voltage)
				_state.last_full_charge_capacity_milliwatthours = (*_state.last_full_charge_capacity_milliamperehours) * (*_state.voltage);
		} else if(*_state.acpi_units == BIF::PowerUnit::MILLIWATT) {
			_state.last_full_charge_capacity_milliwatthours = *last_full_charge_capacity;

			if(_state.voltage)
				_state.last_full_charge_capacity_milliamperehours = (*_state.last_full_charge_capacity_milliwatthours) / (*_state.voltage);
		} else {
			panicLogger() << "thor: unhandled ACPI battery power unit " << *_state.acpi_units << frg::endlog;
		}
	} else {
		_state.last_full_charge_capacity_milliwatthours = frg::null_opt;
		_state.last_full_charge_capacity_milliamperehours = frg::null_opt;
	}

	if(battery_technology) {
		switch(*battery_technology) {
			case BIF::BatteryTechnology::PRIMARY:
			case BIF::BatteryTechnology::SECONDARY:
				_state.battery_technology = *battery_technology;
				break;
			default:
				_state.battery_technology = frg::null_opt;
				break;
		}
	} else {
		_state.battery_technology = frg::null_opt;
	}

	if(design_voltage && *design_voltage != BIF::DesignVoltage::UNKNOWN)
		_state.design_voltage = *design_voltage;
	else
		_state.design_voltage = frg::null_opt;

	uacpi_object_unref(bif);
}

void BatteryBusObject::updateBST() {
	uacpi_object *bst = UACPI_NULL;

	auto ret = uacpi_eval_simple_package(_node, "_BST", &bst);
	if(ret != UACPI_STATUS_OK) {
		infoLogger() << "thor: _BST error " << uacpi_status_to_string(ret) << frg::endlog;
		return;
	}

	uacpi_object_array pkg;
	ret = uacpi_object_get_package(bst, &pkg);
	if(ret != UACPI_STATUS_OK) {
		infoLogger() << "thor: uacpi_object_get_package(bst) error " << uacpi_status_to_string(ret) << frg::endlog;
		return;
	}

	auto battery_state = intFromPackage(pkg, 0);
	auto present_rate = intFromPackage(pkg, 1);
	auto remaining_capacity = intFromPackage(pkg, 2);
	auto present_voltage = intFromPackage(pkg, 3);

	if(!battery_state) {
		infoLogger() << "thor: battery state: invalid" << frg::endlog;
	} else {
		if(*battery_state & BST::State::DISCHARGING)
			_state.charging = false;
		if(*battery_state & BST::State::CHARGING)
			_state.charging = true;
		if(*battery_state & BST::State::CRITICAL_ENERGY_STATE)
			if(logBatteryUpdates)
				infoLogger() << "thor: battery state: critical energy" << frg::endlog;
		if(*battery_state & BST::State::CHARGE_LIMITING)
			if(logBatteryUpdates)
				infoLogger() << "thor: battery state: charge limiting" << frg::endlog;
	}

	if(present_voltage && *present_voltage != BST::Voltage::UNKNOWN)
		_state.voltage = *present_voltage;
	else
		_state.voltage = frg::null_opt;

	if(_state.acpi_units && present_rate && *present_rate != BST::Rate::UNKNOWN) {
		if(*_state.acpi_units == BIF::PowerUnit::MILLIAMPERE) {
			_state.rate_milliampere = frg::abs(uacpi_i32(*present_rate));

			if(_state.voltage)
				_state.rate_milliwatt = (*_state.rate_milliampere) * (*_state.voltage);
			else
				_state.rate_milliwatt = frg::null_opt;
		} else if(*_state.acpi_units == BIF::PowerUnit::MILLIWATT) {
			_state.rate_milliwatt = *present_rate;

			if(_state.voltage)
				_state.rate_milliampere = (*_state.rate_milliwatt) / (*_state.voltage);
			else
				_state.rate_milliampere = frg::null_opt;
		} else {
			panicLogger() << "thor: unhandled ACPI battery power unit " << *_state.acpi_units << frg::endlog;
		}
	} else {
		_state.rate_milliwatt = frg::null_opt;
		_state.rate_milliampere = frg::null_opt;
	}

	if(_state.acpi_units && remaining_capacity && remaining_capacity != BST::Capacity::UNKNOWN) {
		if(*_state.acpi_units == BIF::PowerUnit::MILLIAMPERE) {
			_state.remaining_capacity_milliamperehours = *remaining_capacity;

			if(_state.voltage)
				_state.remaining_capacity_milliwatthours = (*_state.remaining_capacity_milliamperehours) * (*_state.voltage);
		} else if(*_state.acpi_units == BIF::PowerUnit::MILLIWATT) {
			_state.remaining_capacity_milliwatthours = *remaining_capacity;

			if(_state.voltage)
				_state.remaining_capacity_milliamperehours = (*_state.remaining_capacity_milliwatthours) / (*_state.voltage);
		} else {
			panicLogger() << "thor: unhandled ACPI battery power unit " << *_state.acpi_units << frg::endlog;
		}
	} else {
		_state.remaining_capacity_milliwatthours = frg::null_opt;
		_state.remaining_capacity_milliamperehours = frg::null_opt;
	}

	uacpi_object_unref(bst);
}

void BatteryBusObject::updateState() {
	updateBIF();
	// _BST depends on unit information from _BIF
	updateBST();

	if(logBatteryUpdates) {
		infoLogger() << "thor: battery " << _id << " update:" << frg::endlog;
		infoLogger() << "\tState: " << (_state.charging ? "charging" : "discharging") << frg::endlog;
		if(_state.battery_technology)
			infoLogger() << "\tBattery Technology: " << (*_state.battery_technology == BIF::BatteryTechnology::PRIMARY ? "Primary" : "Secondary") << frg::endlog;
		if(_state.design_voltage)
			infoLogger() << "\tDesign Voltage: " << *_state.design_voltage << " mV" << frg::endlog;
		if(_state.voltage)
			infoLogger() << "\tVoltage: " << *_state.voltage << " mV" << frg::endlog;
		if(_state.rate_milliwatt)
			infoLogger() << "\tRate: " << *_state.rate_milliwatt << " mW" << frg::endlog;
		if(_state.rate_milliampere)
			infoLogger() << "\tRate: " << *_state.rate_milliampere << " mA" << frg::endlog;
		if(_state.remaining_capacity_milliwatthours)
			infoLogger() << "\tRemaining Capacity: " << *_state.remaining_capacity_milliwatthours << " mWh" << frg::endlog;
		if(_state.remaining_capacity_milliamperehours)
			infoLogger() << "\tRemaining Capacity: " << *_state.remaining_capacity_milliamperehours << " mAh" << frg::endlog;
		if(_state.design_capacity_milliwatthours)
			infoLogger() << "\tDesign Capacity: " << *_state.design_capacity_milliwatthours << " mWh" << frg::endlog;
		if(_state.design_capacity_milliamperehours)
			infoLogger() << "\tDesign Capacity: " << *_state.design_capacity_milliamperehours << " mAh" << frg::endlog;
		if(_state.last_full_charge_capacity_milliwatthours)
			infoLogger() << "\tLast Full Charge Capacity: " << *_state.last_full_charge_capacity_milliwatthours << " mWh" << frg::endlog;
		if(_state.last_full_charge_capacity_milliamperehours)
			infoLogger() << "\tLast Full Charge Capacity: " << *_state.last_full_charge_capacity_milliamperehours << " mAh" << frg::endlog;
	}
}

void initializeBatteries() {
	async::detach_with_allocator(*kernelAlloc, []() -> coroutine<void> {
		co_await acpiFiber->associatedWorkQueue()->schedule();

		uacpi_find_devices(ACPI_HID_BATTERY, [](void *, uacpi_namespace_node *node, uint32_t) {
			auto bifStatus = uacpi_namespace_node_find(node, "_BIF", nullptr);
			auto bstStatus = uacpi_namespace_node_find(node, "_BST", nullptr);

			if(bifStatus != UACPI_STATUS_OK || bstStatus != UACPI_STATUS_OK)
				return UACPI_ITERATION_DECISION_CONTINUE;

			auto obj = frg::construct<BatteryBusObject>(
				*kernelAlloc, next_battery_id++, node);
			async::detach_with_allocator(*kernelAlloc, obj->run());

			return UACPI_ITERATION_DECISION_CONTINUE;
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
