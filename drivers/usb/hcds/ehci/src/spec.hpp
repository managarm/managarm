
#ifndef EHCI_SPEC_HPP
#define EHCI_SPEC_HPP

#include <arch/register.hpp>
#include <arch/variable.hpp>

//-------------------------------------------------
// registers
//-------------------------------------------------

namespace op_regs {
	arch::bit_register<uint32_t> usbcmd(0);
	arch::bit_register<uint32_t> usbsts(0x04);
	arch::bit_register<uint32_t> usbintr(0x08);
	arch::scalar_register<uint32_t> asynclistaddr(0x18);
	arch::scalar_register<uint32_t> configflag(0x40);
}

namespace cap_regs {
	arch::scalar_register<uint8_t> caplength(0);
	arch::bit_register<uint32_t> hcsparams(0x04);
	arch::bit_register<uint32_t> hccparams(0x08);
}

namespace hcsparams {
	arch::field<uint32_t, uint8_t> nPorts(0, 4);
	arch::field<uint32_t, bool> portPower(4, 1);
}

namespace hccparams {
	arch::field<uint32_t, unsigned int> extPointer(8, 8);
	arch::field<uint32_t, bool> extendedStructs(0, 1);
}

namespace port_regs {
	arch::bit_register<uint32_t> sc(0);
}

namespace usbcmd {
	arch::field<uint32_t, bool> run(0, 1);
	arch::field<uint32_t, bool> hcReset(1, 1);
	arch::field<uint32_t, bool> asyncEnable(5, 1);
	arch::field<uint32_t, uint8_t> irqThreshold(16, 8);
}

namespace usbsts {
	arch::field<uint32_t, bool> transactionIrq(0, 1);
	arch::field<uint32_t, bool> errorIrq(1, 1);
	arch::field<uint32_t, bool> portChange(2, 1);
	arch::field<uint32_t, bool> hostError(4, 1);
	arch::field<uint32_t, bool> hcHalted(12, 1);
}

namespace usbintr {
	arch::field<uint32_t, bool> transaction(0, 1);
	arch::field<uint32_t, bool> usbError(1, 1);
	arch::field<uint32_t, bool> portChange(2, 1);
	arch::field<uint32_t, bool> hostError(4, 1);
}

namespace portsc {
	arch::field<uint32_t, bool> connectStatus(0, 1);
	arch::field<uint32_t, bool> connectChange(1, 1);
	arch::field<uint32_t, bool> enableStatus(2, 1);
	arch::field<uint32_t, bool> enableChange(3, 1);
	arch::field<uint32_t, bool> portReset(8, 1);
	arch::field<uint32_t, uint8_t> lineStatus(10, 2);
	arch::field<uint32_t, bool> portOwner(13, 1);
}

//-------------------------------------------------
// QueueHead
//-------------------------------------------------

namespace qh_horizontal {
	arch::field<uint32_t, bool> terminate(0, 1);
	arch::field<uint32_t, uint8_t> typeSelect(1, 2);
	arch::field<uint32_t, uint32_t> horizontalPtr(0, 32);
}

namespace qh_flags {
	arch::field<uint32_t, uint8_t> deviceAddr(0, 7);
	arch::field<uint32_t, bool> inactivate(7, 1);
	arch::field<uint32_t, uint8_t> endpointNumber(8, 4);
	arch::field<uint32_t, uint8_t> endpointSpeed(12, 2);
	arch::field<uint32_t, bool> manualDataToggle(14, 1);
	arch::field<uint32_t, bool> reclaimHead(15, 1);
	arch::field<uint32_t, unsigned int> maxPacketLength(16, 11);
	arch::field<uint32_t, bool> controlEndpointFlag(27, 1);
	arch::field<uint32_t, uint8_t> nakReload(28, 4);
}

namespace qh_mask {
	arch::field<uint32_t, uint16_t> interruptScheduleMask(0, 8);
	arch::field<uint32_t, uint16_t> multiplier(30, 2);
}

namespace qh_curTd {
	arch::field<uint32_t, uint32_t> curTd(0, 32);
}

namespace qh_nextTd {
	arch::field<uint32_t, bool> terminate(0, 1);
	arch::field<uint32_t, uint32_t> nextTd(0, 32);
}

namespace qh_altTd {
	arch::field<uint32_t, bool> terminate(0, 1);
	arch::field<uint32_t, uint8_t> nakCounter(1, 4);
	arch::field<uint32_t, uint32_t> altTd(0, 32);
}

namespace qh_status {
	arch::field<uint32_t, bool> pingError(0, 1);
	arch::field<uint32_t, bool> splitXState(1, 1);
	arch::field<uint32_t, bool> missedFrame(2, 1);
	arch::field<uint32_t, bool> transactionError(3, 1);
	arch::field<uint32_t, bool> babbleDetected(4, 1);
	arch::field<uint32_t, bool> dataBufferError(5, 1);
	arch::field<uint32_t, bool> halted(6, 1);
	arch::field<uint32_t, bool> active(7, 1);
	arch::field<uint32_t, uint8_t> pidCode(8, 2);
	arch::field<uint32_t, uint8_t> errorCounter(10, 2);
	arch::field<uint32_t, uint8_t> cPage(12, 3);
	arch::field<uint32_t, bool> interruptOnComplete(15, 1);
	arch::field<uint32_t, uint16_t> totalBytes(16, 15);
	arch::field<uint32_t, bool> dataToggle(31, 1);
}

namespace qh_buffer {
	arch::field<uint32_t, uint16_t> curOffset(0, 12);
	arch::field<uint32_t, uint32_t> bufferPtr(0, 32);
}

// The alignment ensures that the struct does not cross a page boundary.
struct alignas(128) QueueHead {
	arch::bit_variable<uint32_t> horizontalPtr;
	arch::bit_variable<uint32_t> flags;
	arch::bit_variable<uint32_t> mask;
	arch::bit_variable<uint32_t> curTd;
	arch::bit_variable<uint32_t> nextTd;
	arch::bit_variable<uint32_t> altTd;
	arch::bit_variable<uint32_t> status;
	arch::bit_variable<uint32_t> bufferPtr0;
	arch::bit_variable<uint32_t> bufferPtr1;
	arch::bit_variable<uint32_t> bufferPtr2;
	arch::bit_variable<uint32_t> bufferPtr3;
	arch::bit_variable<uint32_t> bufferPtr4;
	arch::scalar_variable<uint32_t> extendedPtr0;
	arch::scalar_variable<uint32_t> extendedPtr1;
	arch::scalar_variable<uint32_t> extendedPtr2;
	arch::scalar_variable<uint32_t> extendedPtr3;
	arch::scalar_variable<uint32_t> extendedPtr4;
};

//-------------------------------------------------
// TransferDescriptor
//-------------------------------------------------

namespace td_ptr {
	arch::field<uint32_t, bool> terminate(0, 1);
	arch::field<uint32_t, uint32_t> ptr(0, 32);
}

namespace td_status {
	arch::field<uint32_t, bool> pingError(0, 1);
	arch::field<uint32_t, bool> splitXState(1, 1);
	arch::field<uint32_t, bool> missedFrame(2, 1);
	arch::field<uint32_t, bool> transactionError(3, 1);
	arch::field<uint32_t, bool> babbleDetected(4, 1);
	arch::field<uint32_t, bool> dataBufferError(5, 1);
	arch::field<uint32_t, bool> halted(6, 1);
	arch::field<uint32_t, bool> active(7, 1);
	arch::field<uint32_t, uint8_t> pidCode(8, 2);
	arch::field<uint32_t, uint8_t> errorCounter(10, 2);
	arch::field<uint32_t, uint8_t> cPage(12, 3);
	arch::field<uint32_t, bool> interruptOnComplete(15, 1);
	arch::field<uint32_t, uint16_t> totalBytes(16, 15);
	arch::field<uint32_t, bool> dataToggle(31, 1);
}

namespace td_buffer {
	arch::field<uint32_t, uint16_t> curOffset(0, 12);
	arch::field<uint32_t, uint32_t> bufferPtr(0, 32);
}

// The alignment ensures that the struct does not cross a page boundary.
struct alignas(64) TransferDescriptor {
	arch::bit_variable<uint32_t> nextTd;
	arch::bit_variable<uint32_t> altTd;
	arch::bit_variable<uint32_t> status;
	arch::bit_variable<uint32_t> bufferPtr0;
	arch::bit_variable<uint32_t> bufferPtr1;
	arch::bit_variable<uint32_t> bufferPtr2;
	arch::bit_variable<uint32_t> bufferPtr3;
	arch::bit_variable<uint32_t> bufferPtr4;
	arch::scalar_variable<uint32_t> extendedPtr0;
	arch::scalar_variable<uint32_t> extendedPtr1;
	arch::scalar_variable<uint32_t> extendedPtr2;
	arch::scalar_variable<uint32_t> extendedPtr3;
	arch::scalar_variable<uint32_t> extendedPtr4;
};

#endif // EHCI_SPEC_HPP

