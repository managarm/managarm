
#ifndef XHCI_SPEC_HPP
#define XHCI_SPEC_HPP

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
	arch::field<uint32_t, uint8_t> max_ports(24, 8);
	arch::field<uint32_t, uint16_t> max_intrs(8, 10);
	arch::field<uint32_t, uint8_t> max_dev_slots(0, 8);
}

namespace hcsparams2 {
	arch::field<uint32_t, uint8_t> ist(0, 4);
	arch::field<uint32_t, uint8_t> erst_max(4, 4);
	arch::field<uint32_t, uint8_t> max_scratchpad_bufs_hi(21, 4);
	arch::field<uint32_t, bool> scratchpad_restore(26, 1);
	arch::field<uint32_t, uint8_t> max_scratchpad_bufs_low(27, 4);
}

namespace hccparams1 {
	arch::field<uint32_t, uint16_t> ext_cap_ptr(16, 16);
}

namespace usbcmd {
	arch::field<uint32_t, bool> run(0, 1);
	arch::field<uint32_t, bool> hc_reset(1, 1);
	arch::field<uint32_t, bool> intr_enable(2, 1);
}

namespace usbsts {
	arch::field<uint32_t, bool> hc_halted(0, 1);
	arch::field<uint32_t, bool> host_sysem_err(2, 1);
	arch::field<uint32_t, bool> event_intr(3, 1);
	arch::field<uint32_t, bool> port_change(4, 1);
	arch::field<uint32_t, bool> controller_not_ready(11, 1);
	arch::field<uint32_t, bool> host_controller_error(12, 1);
}

namespace config {
	arch::field<uint32_t, uint8_t> enabled_device_slots(0, 8);
}

namespace interrupter {
	arch::bit_register<uint32_t> iman(0x0);
	arch::scalar_register<uint32_t> imod(0x4);
	arch::scalar_register<uint32_t> erstsz(0x8);
	arch::scalar_register<uint32_t> erstba_low(0x10);
	arch::scalar_register<uint32_t> erstba_hi(0x14);
	arch::scalar_register<uint32_t> erdp_low(0x18);
	arch::scalar_register<uint32_t> erdp_hi(0x1C);
}

namespace iman {
	arch::field<uint32_t, bool> pending(0, 1);
	arch::field<uint32_t, bool> enable(1, 1);
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
	noopCommandnoopCommand,
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


#endif // XHCI_SPEC_HPP
