
#ifndef UART_SPEC_HPP
#define UART_SPEC_HPP

constexpr int COM1 = 0x3F8;
constexpr int COM2 = 0x2F8;
constexpr int COM3 = 0x3E8;
constexpr int COM4 = 0x2E8;

enum BaudRate {
	low9600 = 0x0C,
	high9600 = 0
};

enum class DataBits {
	charLen5 = 0,
	charLen6 = 1,
	charLen7 = 2,
	charLen8 = 3
};

enum class StopBits {
	one = 0,
	two = 1
};

enum class Parity {
	none = 0,
	odd = 1,
	even = 3,
	mark = 5,
	space = 7
};

enum class Irq {
	dataAvailable = 0,
	transmitEmpty = 1,
	error = 2,
	statusChange = 3,
};

enum class FifoCtrl {
	disable = 0,
	enable = 1,
	triggerLvl1 = 0,
	triggerLvl4 = 1,
	triggerLvl8 = 2,
	triggerLvl14 = 3,
};

enum class IrqCtrl {
	disable = 0,
	enable = 1	
};

enum class IrqIds {
	lineStatus = 3,
	dataAvailable = 2,
	charTimeout = 6,
	txEmpty = 1,
	modem = 0
};

namespace uart_register {
	arch::scalar_register<uint8_t> data(0);
	arch::bit_register<uint8_t> irqEnable(1);
	arch::scalar_register<uint8_t> baudLow(0);
	arch::scalar_register<uint8_t> baudHigh(1);
	arch::bit_register<uint8_t> irqIdentification(2);
	arch::bit_register<uint8_t> fifoControl(2);
	arch::bit_register<uint8_t> lineControl(3);
	arch::bit_register<uint8_t> lineStatus(5);
}

namespace irq_enable {
	arch::field<uint8_t, IrqCtrl> dataAvailable(0, 1);
	arch::field<uint8_t, IrqCtrl> txEmpty(1, 1);
	arch::field<uint8_t, IrqCtrl> lineStatus(2, 1);
	arch::field<uint8_t, IrqCtrl> modem(3, 1);
}

namespace fifo_control {
	arch::field<uint8_t, FifoCtrl> fifoEnable(0, 1);
	arch::field<uint8_t, FifoCtrl> fifoIrqLvl(6, 2);
}

namespace line_control {
	arch::field<uint8_t, DataBits> dataBits(0, 2);
	arch::field<uint8_t, StopBits> stopBit(2, 1);
	arch::field<uint8_t, Parity> parityBits(3, 3);
	arch::field<uint8_t, bool> dlab(7, 1);
}

namespace line_status {
	arch::field<uint8_t, bool> dataReady(0, 1);
	arch::field<uint8_t, bool> txReady(5, 1);
}

namespace irq_ident_register {
	arch::field<uint8_t, bool> ignore(0, 1);
	arch::field<uint8_t, IrqIds> id(1, 3);
}

#endif // UART_SPEC_HPP


