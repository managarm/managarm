#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <uart/samsung.hpp>

namespace common::uart {

namespace {

constexpr arch::bit_register<uint32_t> status{0x10};
constexpr arch::scalar_register<uint32_t> data{0x20};

constexpr arch::field<uint32_t, bool> txBufferEmpty{1, 1};

} // anonymous namespace

Samsung::Samsung(uintptr_t base) : base_{base}, space_{base} {}

void Samsung::write(char c) {
	while (!(space_.load(status) & txBufferEmpty)) {
		// Do nothing until the UART is ready to transmit.
	}

	space_.store(data, c);
}

} // namespace common::uart
