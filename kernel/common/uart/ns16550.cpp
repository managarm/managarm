#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <uart/ns16550.hpp>

namespace common::uart {

namespace {

constexpr arch::scalar_register<uint8_t> data(0);
constexpr arch::scalar_register<uint8_t> baudLow(0);
constexpr arch::scalar_register<uint8_t> interruptEnable(1);
constexpr arch::scalar_register<uint8_t> baudHigh(1);
constexpr arch::bit_register<uint8_t> fifoControl(2);
constexpr arch::bit_register<uint8_t> lineControl(3);
constexpr arch::bit_register<uint8_t> modemControl(4);
constexpr arch::bit_register<uint8_t> lineStatus(5);

constexpr arch::field<uint8_t, bool> txReady(5, 1);

constexpr arch::field<uint8_t, int> dataBits(0, 2);
constexpr arch::field<uint8_t, bool> stopBit(2, 1);
constexpr arch::field<uint8_t, int> parityBits(3, 3);
constexpr arch::field<uint8_t, bool> dlab(7, 1);

constexpr arch::field<uint8_t, bool> enableFifos(0, 1);
constexpr arch::field<uint8_t, bool> clearRxFifo(1, 1);
constexpr arch::field<uint8_t, bool> clearTxFifo(2, 1);

constexpr arch::field<uint8_t, bool> dtr(0, 1);
constexpr arch::field<uint8_t, bool> rts(1, 1);

} // anonymous namespace

template <typename Space>
Ns16550<Space>::Ns16550(Space regs) : regs_{regs} {
	// Disable all interrupts.
	regs_.store(interruptEnable, 0);

	// Set the baud rate.
	regs_.store(lineControl, dlab(true));
	regs_.store(baudLow, 0x01);
	regs_.store(baudHigh, 0x00);

	// Configure: 8 data bits, 1 stop bit, no parity.
	regs_.store(lineControl, dataBits(3) | stopBit(0) | parityBits(0) | dlab(false));

	// Clear and enable FIFOs.
	regs_.store(fifoControl, enableFifos(true) | clearRxFifo(true) | clearTxFifo(true));

	// Clear DTR + RTS.
	regs_.store(modemControl, dtr(false) | rts(false));
}

template <typename Space>
void Ns16550<Space>::write(char c) {
	while (!(regs_.load(lineStatus) & txReady)) {
		// Do nothing until the UART is ready to transmit.
	}
	regs_.store(data, c);
}

template struct Ns16550<arch::io_space>;
template struct Ns16550<arch::mem_space>;

} // namespace common::uart
