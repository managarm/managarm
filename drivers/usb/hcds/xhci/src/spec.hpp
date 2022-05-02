#pragma once

#include <arch/register.hpp>
#include <arch/variable.hpp>

//-------------------------------------------------
// registers
//-------------------------------------------------

namespace op_regs {
	arch::bit_register<uint32_t> usbcmd(0);
	arch::bit_register<uint32_t> usbsts(0x04);
	arch::scalar_register<uint32_t> pagesize(0x8);
	arch::scalar_register<uint32_t> dnctrl(0x14);
	arch::scalar_register<uint64_t> crcr(0x18);
	arch::scalar_register<uint64_t> dcbaap(0x30);
	arch::bit_register<uint32_t> config(0x38);
}

namespace cap_regs {
	arch::scalar_register<uint8_t> caplength(0);
	arch::scalar_register<uint16_t> hciversion(0x02);
	arch::bit_register<uint32_t> hcsparams1(0x04);
	arch::bit_register<uint32_t> hcsparams2(0x08);
	arch::bit_register<uint32_t> hcsparams3(0x0C);
	arch::bit_register<uint32_t> hccparams1(0x10);
	arch::scalar_register<uint32_t> dboff(0x14);
	arch::scalar_register<uint32_t> rtsoff(0x18);
	arch::bit_register<uint32_t> hccparams2(0x1C);
}

namespace hcsparams1 {
	arch::field<uint32_t, uint8_t> maxPorts(24, 8);
	arch::field<uint32_t, uint16_t> maxIntrs(8, 10);
	arch::field<uint32_t, uint8_t> maxDevSlots(0, 8);
}

namespace hcsparams2 {
	arch::field<uint32_t, uint8_t> ist(0, 4);
	arch::field<uint32_t, uint8_t> erstMax(4, 4);
	arch::field<uint32_t, uint8_t> maxScratchpadBufsHi(21, 4);
	arch::field<uint32_t, bool> scratchpadRestore(26, 1);
	arch::field<uint32_t, uint8_t> maxScratchpadBufsLow(27, 4);
}

namespace hccparams1 {
	arch::field<uint32_t, uint16_t> extCapPtr(16, 16);
	arch::field<uint32_t, bool> contextSize(2, 1);
}

namespace usbcmd {
	arch::field<uint32_t, bool> run(0, 1);
	arch::field<uint32_t, bool> hcReset(1, 1);
	arch::field<uint32_t, bool> intrEnable(2, 1);
}

namespace usbsts {
	arch::field<uint32_t, bool> hcHalted(0, 1);
	arch::field<uint32_t, bool> hostSystemErr(2, 1);
	arch::field<uint32_t, bool> eventIntr(3, 1);
	arch::field<uint32_t, bool> portChange(4, 1);
	arch::field<uint32_t, bool> controllerNotReady(11, 1);
	arch::field<uint32_t, bool> hostControllerError(12, 1);
}

namespace config {
	arch::field<uint32_t, uint8_t> enabledDeviceSlots(0, 8);
}

namespace interrupter {
	arch::bit_register<uint32_t> iman(0x0);
	arch::scalar_register<uint32_t> imod(0x4);
	arch::scalar_register<uint32_t> erstsz(0x8);
	arch::scalar_register<uint32_t> erstbaLow(0x10);
	arch::scalar_register<uint32_t> erstbaHi(0x14);
	arch::scalar_register<uint32_t> erdpLow(0x18);
	arch::scalar_register<uint32_t> erdpHi(0x1C);
}

namespace iman {
	arch::field<uint32_t, bool> pending(0, 1);
	arch::field<uint32_t, bool> enable(1, 1);
}

namespace port {
	arch::bit_register<uint32_t> portsc(0x0);
	arch::bit_register<uint32_t> portpmsc(0x4);
	arch::bit_register<uint32_t> portli(0x8);
	arch::bit_register<uint32_t> porthlpmc(0xC);
}

namespace portsc {
	arch::field<uint32_t, bool> portReset(4, 1);
	arch::field<uint32_t, bool> portEnable(1, 1);
	arch::field<uint32_t, bool> connectStatus(0, 1);
	arch::field<uint32_t, bool> portPower(9, 1);
	arch::field<uint32_t, uint8_t> portLinkStatus(5, 4);
	arch::field<uint32_t, bool> portLinkStatusStrobe(16, 1);
	arch::field<uint32_t, uint8_t> portSpeed(10, 4);

	arch::field<uint32_t, bool> connectStatusChange(17, 1);
	arch::field<uint32_t, bool> portResetChange(21, 1);
	arch::field<uint32_t, bool> portEnableChange(18, 1);
	arch::field<uint32_t, bool> warmPortResetChange(19, 1);
	arch::field<uint32_t, bool> overCurrentChange(20, 1);
	arch::field<uint32_t, bool> portLinkStatusChange(22, 1);
	arch::field<uint32_t, bool> portConfigErrorChange(23, 1);
}

struct RawTrb {
	uint32_t val[4];
};
static_assert(sizeof(RawTrb) == 16, "invalid trb size");

enum class TrbType : uint8_t {
	reserved = 0,

	// transfer ring things
	normal,
	setupStage,
	dataStage,
	statusStage,
	isoch,
	link, // also applies to command ring
	eventData,
	noop,

	// command ring things
	enableSlotCommand,
	disableSlotCommand,
	addressDeviceCommand,
	configureEndpointCommand,
	evalContextCommand,
	resetEndpointCommand,
	stopEndpointCommand,
	setTrDequeuePtrCommand,
	resetDeviceCommand,
	forceEventCommand,
	negotiateBandwidthCommand,
	setLatencyToleranceValCommand,
	getPortBandwidthCommand,
	forceHeaderCommand,
	noopCommand,
	getExtPropertyCommand,
	setExtPropertyCommand,

	// event ring things
	transferEvent = 32,
	commandCompletionEvent,
	portStatusChangeEvent,
	bandwidthRequestEvent,
	doorbellEvent,
	hostControllerEvent,
	deviceNotificationEvent,
	mfindexWrapEvent
};

struct InputControlContext {
	uint32_t dropContextFlags;
	uint32_t addContextFlags;
	uint32_t rsvd1[5];
	uint32_t configValue : 8;
	uint32_t interfaceNumber : 8;
	uint32_t alternateSetting : 8;
	uint32_t rsvd2 : 8;
};
static_assert(sizeof(InputControlContext) == 32, "invalid InputControlContext size");

struct RawContext {
	uint32_t val[8];
};

struct alignas(64) InputContext {
	InputControlContext icc;
	RawContext slotContext;
	RawContext endpointContext[31];
};
static_assert (sizeof(InputContext) == 34 * 32, "invalid InputContext size"); // 34 due to 64 byte alignment

struct alignas(64) DeviceContext {
	RawContext slotContext;
	RawContext endpointContext[31];
};
static_assert (sizeof(DeviceContext) == 32 * 32, "invalid DeviceContext size");
