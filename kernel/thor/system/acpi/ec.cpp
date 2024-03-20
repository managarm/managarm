#include <thor-internal/main.hpp>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/arch/ints.hpp>
#include <stdlib.h>

#include <uacpi/kernel_api.h>
#include <uacpi/acpi.h>
#include <uacpi/uacpi.h>
#include <uacpi/utilities.h>
#include <uacpi/tables.h>
#include <uacpi/event.h>
#include <uacpi/resources.h>
#include <uacpi/io.h>
#include <uacpi/opregion.h>

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

	acpi_gas control;
	acpi_gas data;

	void pollIbf() {
		waitForBit(control, EC_IBF, false);
	}

	void pollObf() {
		waitForBit(control, EC_OBF, true);
	}

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
};
static frg::manual_box<ECDevice> ecDevice;

static uacpi_status ecDoRw(uacpi_region_op op, uacpi_region_rw_data *data) {
	auto *ecDevice = reinterpret_cast<ECDevice *>(data->handler_context);

	if(data->byte_width != 1) {
		infoLogger() << "thor: invalid EC access width " << data->byte_width << frg::endlog;
		return UACPI_STATUS_INVALID_ARGUMENT;
	}

	ecDevice->burstEnable();

	switch(op) {
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
	switch(op) {
		case UACPI_REGION_OP_ATTACH:
		case UACPI_REGION_OP_DETACH:
			return UACPI_STATUS_OK;
		default:
			return ecDoRw(op, reinterpret_cast<uacpi_region_rw_data *>(op_data));
	}
}

static bool initFromEcdt() {
	uacpi_table *ecdtTbl;

	auto ret = uacpi_table_find_by_signature(makeSignature("ECDT"), &ecdtTbl);
	if(ret != UACPI_STATUS_OK) {
		infoLogger() << "thor: no ECDT detected" << frg::endlog;
		return false;
	}

	auto *ecdt = reinterpret_cast<acpi_ecdt *>(ecdtTbl->hdr);
	infoLogger() << "thor: found ECDT, EC@" << ecdt->ec_id << frg::endlog;

	auto *ecNode = uacpi_namespace_node_find(UACPI_NULL, ecdt->ec_id);
	if(ecNode == UACPI_NULL) {
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
	uacpi_find_devices("PNP0C09", [](void*, uacpi_namespace_node *node) {
		uacpi_resources resources;

		auto ret = uacpi_get_current_resources(node, &resources);
		if(ret != UACPI_STATUS_OK)
			return UACPI_NS_ITERATION_DECISION_CONTINUE;

		struct initCtx {
			acpi_gas control, data;
			size_t idx;
		} ctx {};

		ret = uacpi_for_each_resource(&resources,
			[](void *opaque, uacpi_resource *res){
				auto *ctx = reinterpret_cast<initCtx*>(opaque);
				auto *reg = ctx->idx ? &ctx->control : &ctx->data;

				switch(res->type) {
					case UACPI_RESOURCE_TYPE_IO:
						reg->address = res->io.minimum;
						reg->register_bit_width = res->io.length * 8;
						break;
					case UACPI_RESOURCE_TYPE_FIXED_IO:
						reg->address = res->fixed_io.address;
						reg->register_bit_width = res->fixed_io.length * 8;
						break;
					default:
						return UACPI_RESOURCE_ITERATION_CONTINUE;
				}

				reg->address_space_id = UACPI_ADDRESS_SPACE_SYSTEM_IO;

				if(++ctx->idx == 2)
					return UACPI_RESOURCE_ITERATION_ABORT;
				return UACPI_RESOURCE_ITERATION_CONTINUE;
			}, &ctx);
		uacpi_kernel_free(resources.head);

		if(ctx.idx != 2) {
			infoLogger() << "thor: didn't find all needed resources for EC" << frg::endlog;
			return UACPI_NS_ITERATION_DECISION_CONTINUE;
		}

		ecDevice.initialize();
		ecDevice->node = node;
		ecDevice->control = ctx.control;
		ecDevice->data = ctx.data;

		auto *full_path = uacpi_namespace_node_generate_absolute_path(node);
		infoLogger() << "thor: found an EC@" << full_path << frg::endlog;
		uacpi_kernel_free(const_cast<char*>(full_path));

		return UACPI_NS_ITERATION_DECISION_BREAK;
	}, nullptr);
}

void initEc() {
	if(!initFromEcdt())
		initFromNamespace();

	if(!ecDevice) {
		infoLogger() << "thor: no EC devices on the system" << frg::endlog;
		return;
	}

	uacpi_install_address_space_handler(
		ecDevice->node, UACPI_ADDRESS_SPACE_EMBEDDED_CONTROLLER,
		handleEcRegion, ecDevice.get()
	);

	uint64_t value = 0;
	uacpi_eval_integer(ecDevice->node, "_GLK", UACPI_NULL, &value);
	if(value)
		infoLogger() << "thor: EC requires locking (this is a TODO)" << frg::endlog;
}

}
