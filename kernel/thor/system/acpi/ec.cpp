#include <stdlib.h>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/main.hpp>

#include <frg/optional.hpp>

#include <uacpi/acpi.h>
#include <uacpi/event.h>
#include <uacpi/io.h>
#include <uacpi/kernel_api.h>
#include <uacpi/notify.h>
#include <uacpi/opregion.h>
#include <uacpi/resources.h>
#include <uacpi/sleep.h>
#include <uacpi/tables.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>

namespace thor::acpi {

static void regWrite(acpi_gas &gas, uint8_t value) {
	auto ret = uacpi_gas_write(&gas, value);
	assert(ret == UACPI_STATUS_OK);
}

uint8_t regRead(acpi_gas &gas) {
	uint64_t reg;

	auto ret = uacpi_gas_read(&gas, &reg);
	assert(ret == UACPI_STATUS_OK);

	return reg;
}

void waitForBit(acpi_gas &gas, uint8_t bit, bool value) {
	uint8_t reg;

	do {
		reg = regRead(gas);
	} while ((reg & bit) != value);
}

#define EC_OBF (1 << 0)
#define EC_IBF (1 << 1)
#define EC_BURST (1 << 4)
#define EC_SCI_EVT (1 << 5)

#define RD_EC 0x80
#define WR_EC 0x81
#define BE_EC 0x82
#define BD_EC 0x83
#define QR_EC 0x84

#define BURST_ACK 0x90

struct ECDevice {
	uacpi_namespace_node *node;
	uacpi_namespace_node *gpeNode;
	frg::optional<uint16_t> gpeIdx;
	bool initialized = false;
	IrqSpinlock lock;

	acpi_gas control;
	acpi_gas data;

	void pollIbf() { waitForBit(control, EC_IBF, false); }

	void pollObf() { waitForBit(control, EC_OBF, true); }

	void writeOne(acpi_gas &reg, uint8_t value) {
		pollIbf();
		regWrite(reg, value);
	}

	uint8_t readOne(acpi_gas &reg) {
		pollObf();
		return regRead(reg);
	}

	void burstEnable() {
		writeOne(control, BE_EC);
		auto ec_ret = readOne(data);
		assert(ec_ret == BURST_ACK);
	}

	void burstDisable() {
		writeOne(control, BD_EC);
		waitForBit(control, EC_BURST, false);
	}

	uint8_t read(uint8_t offset) {
		writeOne(control, RD_EC);
		writeOne(data, offset);
		return readOne(data);
	}

	void write(uint8_t offset, uint8_t value) {
		writeOne(control, WR_EC);
		writeOne(data, offset);
		writeOne(data, value);
	}

	bool checkEvent(uint8_t &idx) {
		auto status = regRead(control);

		// We get an extra EC event when disabling burst, that's ok.
		if (!(status & EC_SCI_EVT))
			return false;

		burstEnable();
		writeOne(control, QR_EC);
		idx = readOne(data);
		burstDisable();

		return true;
	}
};
static frg::manual_box<ECDevice> ecDevice;

static uacpi_status ecDoRw(uacpi_region_op op, uacpi_region_rw_data *data) {
	auto *ecDevice = reinterpret_cast<ECDevice *>(data->handler_context);

	if (data->byte_width != 1) {
		infoLogger() << "thor: invalid EC access width " << data->byte_width << frg::endlog;
		return UACPI_STATUS_INVALID_ARGUMENT;
	}

	auto guard = frg::guard(&ecDevice->lock);

	ecDevice->burstEnable();

	switch (op) {
		case UACPI_REGION_OP_READ:
			data->value = ecDevice->read(data->offset);
			break;
		case UACPI_REGION_OP_WRITE:
			ecDevice->write(data->offset, data->value);
			break;
		default:
			return UACPI_STATUS_INVALID_ARGUMENT;
	}

	ecDevice->burstDisable();
	return UACPI_STATUS_OK;
}

static uacpi_status handleEcRegion(uacpi_region_op op, uacpi_handle op_data) {
	switch (op) {
		case UACPI_REGION_OP_ATTACH:
		case UACPI_REGION_OP_DETACH:
			return UACPI_STATUS_OK;
		default:
			return ecDoRw(op, reinterpret_cast<uacpi_region_rw_data *>(op_data));
	}
}

struct ECQuery {
	uint8_t idx;
	ECDevice *device;
};

void handleEcQuery(uacpi_handle opaque) {
	static const char *hexChars = "0123456789ABCDEF";

	auto *query = reinterpret_cast<ECQuery *>(opaque);
	char methodName[5] = {'_', 'Q', hexChars[(query->idx >> 4) & 0xF], hexChars[query->idx & 0xF]};

	infoLogger() << "thor: evaluating EC query " << methodName << frg::endlog;

	uacpi_eval(query->device->node, methodName, UACPI_NULL, UACPI_NULL);
	uacpi_finish_handling_gpe(query->device->gpeNode, *query->device->gpeIdx);

	frg::destruct(*kernelAlloc, query);
}

uacpi_interrupt_ret handleEcEvent(uacpi_handle ctx, uacpi_namespace_node *, uacpi_u16) {
	auto *ecDevice = reinterpret_cast<ECDevice *>(ctx);
	uacpi_interrupt_ret ret = UACPI_GPE_REENABLE | UACPI_INTERRUPT_HANDLED;

	auto guard = frg::guard(&ecDevice->lock);

	uint8_t idx;
	if (!ecDevice->checkEvent(idx))
		return ret;

	if (idx == 0) {
		infoLogger() << "thor: EC indicates no outstanding events" << frg::endlog;
		return ret;
	}

	infoLogger() << "thor: scheduling EC event " << idx << " for execution" << frg::endlog;

	auto *query = frg::construct<ECQuery>(*kernelAlloc);
	query->device = ecDevice;
	query->idx = idx;
	uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, handleEcQuery, query);

	// Don't re-enable the event handling here, it will be enabled asynchronously
	return UACPI_INTERRUPT_HANDLED;
}

static bool initFromEcdt() {
	uacpi_table ecdtTbl;

	auto ret = uacpi_table_find_by_signature("ECDT", &ecdtTbl);
	if (ret != UACPI_STATUS_OK) {
		infoLogger() << "thor: no ECDT detected" << frg::endlog;
		return false;
	}

	auto *ecdt = reinterpret_cast<acpi_ecdt *>(ecdtTbl.hdr);
	infoLogger() << "thor: found ECDT, EC@" << ecdt->ec_id << frg::endlog;

	uacpi_namespace_node *ecNode = nullptr;
	ret = uacpi_namespace_node_find(UACPI_NULL, ecdt->ec_id, &ecNode);
	if (ret != UACPI_STATUS_OK) {
		infoLogger() << "thor: invalid EC path " << ecdt->ec_id << frg::endlog;
		return false;
	}

	ecDevice.initialize();
	ecDevice->node = ecNode;
	ecDevice->control = ecdt->ec_control;
	ecDevice->data = ecdt->ec_data;
	return true;
}

static void initFromNamespace() {
	uacpi_find_devices(
	    ACPI_HID_EC,
	    [](void *, uacpi_namespace_node *node, uint32_t) {
		    uacpi_resources *resources;

		    auto ret = uacpi_get_current_resources(node, &resources);
		    if (ret != UACPI_STATUS_OK)
			    return UACPI_ITERATION_DECISION_CONTINUE;

		    struct initCtx {
			    acpi_gas control, data;
			    size_t idx;
		    } ctx{};

		    ret = uacpi_for_each_resource(
		        resources,
		        [](void *opaque, uacpi_resource *res) {
			        auto *ctx = reinterpret_cast<initCtx *>(opaque);
			        auto *reg = ctx->idx ? &ctx->control : &ctx->data;

			        switch (res->type) {
				        case UACPI_RESOURCE_TYPE_IO:
					        reg->address = res->io.minimum;
					        reg->register_bit_width = res->io.length * 8;
					        break;
				        case UACPI_RESOURCE_TYPE_FIXED_IO:
					        reg->address = res->fixed_io.address;
					        reg->register_bit_width = res->fixed_io.length * 8;
					        break;
				        default:
					        return UACPI_ITERATION_DECISION_CONTINUE;
			        }

			        reg->address_space_id = UACPI_ADDRESS_SPACE_SYSTEM_IO;

			        if (++ctx->idx == 2)
				        return UACPI_ITERATION_DECISION_BREAK;
			        return UACPI_ITERATION_DECISION_CONTINUE;
		        },
		        &ctx
		    );
		    uacpi_free_resources(resources);

		    if (ctx.idx != 2) {
			    infoLogger() << "thor: didn't find all needed resources for EC" << frg::endlog;
			    return UACPI_ITERATION_DECISION_CONTINUE;
		    }

		    ecDevice.initialize();
		    ecDevice->node = node;
		    ecDevice->control = ctx.control;
		    ecDevice->data = ctx.data;

		    auto *full_path = uacpi_namespace_node_generate_absolute_path(node);
		    infoLogger() << "thor: found an EC@" << full_path << frg::endlog;
		    uacpi_kernel_free(const_cast<char *>(full_path));

		    return UACPI_ITERATION_DECISION_BREAK;
	    },
	    nullptr
	);
}

static void installEcHandlers() {
	uacpi_install_address_space_handler(
	    ecDevice->node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER, handleEcRegion, ecDevice.get()
	);

	uint64_t value = 0;
	uacpi_eval_simple_integer(ecDevice->node, "_GLK", &value);
	if (value)
		infoLogger() << "thor: EC requires locking (this is a TODO)" << frg::endlog;

	auto ret = uacpi_eval_simple_integer(ecDevice->node, "_GPE", &value);
	if (ret != UACPI_STATUS_OK) {
		infoLogger() << "thor: EC has no associated _GPE" << frg::endlog;
		return;
	}

	ecDevice->gpeIdx = value;
	ret = uacpi_install_gpe_handler(
	    nullptr, *ecDevice->gpeIdx, UACPI_GPE_TRIGGERING_EDGE, handleEcEvent, ecDevice.get()
	);
	assert(ret == UACPI_STATUS_OK);

	ecDevice->initialized = true;
}

void initEc() {
	bool earlyReg = true;

	if (!initFromEcdt()) {
		earlyReg = false;
		initFromNamespace();
	}

	if (!ecDevice) {
		infoLogger() << "thor: no EC devices on the system" << frg::endlog;
		return;
	}

	/*
	 * Don't attempt to run _REG early if firmware didn't explicitly ask for
	 * it in the form of providing an ECDT table. It might rely on the namespace
	 * being fully initialized in the _REG method(s).
	 */
	if (earlyReg)
		installEcHandlers();
}

static void asyncShutdown(uacpi_handle) {
	infoLogger() << "thor: shutting down..." << frg::endlog;

	auto ret = uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
	if (ret != UACPI_STATUS_OK) {
		infoLogger() << "thor: shutdown preparation failed" << frg::endlog;
	}

	disableInts();
	ret = uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
	assert(ret == UACPI_STATUS_OK);
}

static uacpi_interrupt_ret handlePowerButton(uacpi_handle) {
	infoLogger() << "thor: scheduling shut down because of power button press" << frg::endlog;

	/*
	 * This must be executed outside of interrupt context because this
	 * potentially requires quite a lot of work, involving sending more
	 * interrupts, acquiring mutexes, sleeping, etc.
	 */
	uacpi_kernel_schedule_work(UACPI_WORK_GPE_EXECUTION, asyncShutdown, nullptr);
	return UACPI_INTERRUPT_HANDLED;
}

static uacpi_status handlePowerButtonNotify(uacpi_handle, uacpi_namespace_node *, uacpi_u64 value) {
	// 0x80: S0 Power Button Pressed
	if (value != 0x80) {
		infoLogger() << "thor: ignoring unknown power button notify value " << value << frg::endlog;
		return UACPI_STATUS_OK;
	}

	infoLogger() << "thor: shutting down because of power button notification" << frg::endlog;

	// We're already in an async callback, so no need to schedule this. Just call right away.
	asyncShutdown(nullptr);

	return UACPI_STATUS_OK;
}

void initEvents() {
	/*
	 * We don't have any sort of power management subsystem,
	 * so just enable all GPEs that have an AML handler.
	 */
	uacpi_finalize_gpe_initialization();

	if (ecDevice) {
		if (!ecDevice->initialized)
			installEcHandlers();

		if (ecDevice->gpeIdx) {
			infoLogger() << "thor: enabling EC GPE " << *ecDevice->gpeIdx << frg::endlog;
			uacpi_enable_gpe(ecDevice->gpeNode, *ecDevice->gpeIdx);
		}
	}

	uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, handlePowerButton, nullptr);

	/*
	 * Modern hardware uses power button devices instead of the fixed event.
	 * Search for them here and hook AML notifications.
	 */
	uacpi_find_devices(
	    ACPI_HID_POWER_BUTTON,
	    [](void *, uacpi_namespace_node *node, uint32_t) {
		    uacpi_install_notify_handler(node, handlePowerButtonNotify, nullptr);
		    return UACPI_ITERATION_DECISION_CONTINUE;
	    },
	    nullptr
	);
}

} // namespace thor::acpi
