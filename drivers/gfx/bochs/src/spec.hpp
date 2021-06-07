#pragma once

#include <arch/register.hpp>
#include <arch/variable.hpp>

enum class RegisterIndex : uint16_t {
	id = 0,
	resX = 1,
	resY = 2,
	bpp = 3,
	enable = 4,
	bank = 5,
	virtWidth = 6,
	virtHeight = 7,
	offX = 8,
	offY = 9
};

namespace enable_bits {
	inline constexpr uint16_t enable = 0x01;
	inline constexpr uint16_t lfb = 0x40;
	inline constexpr uint16_t noMemClear = 0x80;
};

//-------------------------------------------------
// registers
//-------------------------------------------------

namespace regs {
	arch::scalar_register<uint16_t> index(0x01CE);
	arch::scalar_register<uint16_t> data(0x01CF);
}
