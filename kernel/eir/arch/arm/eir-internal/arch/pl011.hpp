#pragma once

#include <arch/mem_space.hpp>
#include <arch/register.hpp>
#include <stdint.h>

namespace eir {

namespace pl011_reg {
static constexpr arch::scalar_register<uint32_t> data{0x00};
static constexpr arch::bit_register<uint32_t> status{0x18};
static constexpr arch::scalar_register<uint32_t> i_baud{0x24};
static constexpr arch::scalar_register<uint32_t> f_baud{0x28};
static constexpr arch::bit_register<uint32_t> control{0x30};
static constexpr arch::bit_register<uint32_t> line_control{0x2c};
static constexpr arch::scalar_register<uint32_t> int_clear{0x44};
} // namespace pl011_reg

namespace pl011_status {
static constexpr arch::field<uint32_t, bool> tx_full{5, 1};
};

namespace pl011_control {
static constexpr arch::field<uint32_t, bool> rx_en{9, 1};
static constexpr arch::field<uint32_t, bool> tx_en{8, 1};
static constexpr arch::field<uint32_t, bool> uart_en{0, 1};
}; // namespace pl011_control

namespace pl011_line_control {
static constexpr arch::field<uint32_t, uint8_t> word_len{5, 2};
static constexpr arch::field<uint32_t, bool> fifo_en{4, 1};
} // namespace pl011_line_control

struct PL011 {
	PL011(uintptr_t base, uint64_t clock) : space_{base}, clock_{clock} {}

	void disable() { space_.store(pl011_reg::control, pl011_control::uart_en(false)); }

	void init(uint64_t baud) {
		disable();

		uint64_t int_part = clock_ / (16 * baud);

		// 3 decimal places of precision should be enough :^)
		uint64_t frac_part =
		    (((clock_ * 1000) / (16 * baud) - (int_part * 1000)) * 64 + 500) / 1000;

		space_.store(pl011_reg::i_baud, int_part);
		space_.store(pl011_reg::f_baud, frac_part);

		// 8n1, fifo enabled
		space_.store(
		    pl011_reg::line_control,
		    pl011_line_control::word_len(3) | pl011_line_control::fifo_en(true)
		);
		space_.store(
		    pl011_reg::control,
		    pl011_control::rx_en(true) | pl011_control::tx_en(true) | pl011_control::uart_en(true)
		);
	}

	void send(uint8_t val) {
		while (space_.load(pl011_reg::status) & pl011_status::tx_full)
			;

		space_.store(pl011_reg::data, val);
	}

private:
	arch::mem_space space_;
	uint64_t clock_;
};

} // namespace eir
