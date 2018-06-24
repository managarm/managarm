
#ifndef UHCI_SPEC_HPP
#define UHCI_SPEC_HPP

#include <arch/register.hpp>
#include <arch/variable.hpp>

//-------------------------------------------------
// registers
//-------------------------------------------------

namespace op_regs {
	arch::bit_register<uint16_t> command(0x00);
	arch::bit_register<uint16_t> status(0x02);
	arch::bit_register<uint16_t> irqEnable(0x04);
	arch::scalar_register<uint16_t> frameNumber(0x06);
	arch::scalar_register<uint32_t> frameListBase(0x08);
}

namespace port_regs {
	arch::bit_register<uint16_t> statusCtrl(0x00);
}

namespace command {
	arch::field<uint16_t, bool> runStop(0, 1);
	arch::field<uint16_t, bool> hostReset(1, 1);
}

namespace status {
	arch::field<uint16_t, bool> transactionIrq(0, 1);
	arch::field<uint16_t, bool> errorIrq(1, 1);
	arch::field<uint16_t, bool> hostSystemError(3, 1);
	arch::field<uint16_t, bool> hostProcessError(4, 1);
}

namespace irq {
	arch::field<uint16_t, bool> timeout(0, 1);
	arch::field<uint16_t, bool> resume(1, 1);
	arch::field<uint16_t, bool> transaction(2, 1);
	arch::field<uint16_t, bool> shortPacket(3, 1);
}

namespace port_status_ctrl {
	arch::field<uint16_t, bool> connectStatus(0, 1);
	arch::field<uint16_t, bool> connectChange(1, 1);
	arch::field<uint16_t, bool> enableStatus(2, 1);
	arch::field<uint16_t, bool> enableChange(3, 1);
	arch::field<uint16_t, bool> lowSpeed(8, 1);
	arch::field<uint16_t, bool> portReset(9, 1);
}

#endif // UHCI_SPEC_HPP

