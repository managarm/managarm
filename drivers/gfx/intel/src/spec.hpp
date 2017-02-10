
#include <arch/register.hpp>

// ----------------------------------------------------------------------------
// Clock (aka DPLL) registers.
// ----------------------------------------------------------------------------

namespace regs {
	static constexpr arch::bit_register<uint32_t> vgaPllDivisor1(0x6000);
	static constexpr arch::bit_register<uint32_t> vgaPllDivisor2(0x6004);
	static constexpr arch::bit_register<uint32_t> vgaPllPost(0x6010);
	static constexpr arch::bit_register<uint32_t> pllControl(0x6014);
	static constexpr arch::bit_register<uint32_t> busMultiplier(0x601C);
	static constexpr arch::bit_register<uint32_t> pllDivisor1(0x6040);
	static constexpr arch::bit_register<uint32_t> pllDivisor2(0x6044);
}

namespace pll_control {
	static constexpr arch::field<uint32_t, unsigned int> phase(9, 4);
	static constexpr arch::field<uint32_t, unsigned int> encodedP1(16, 8);
	static constexpr arch::field<uint32_t, unsigned int> modeSelect(26, 2);
	static constexpr arch::field<uint32_t, bool> disableVga(28, 1);
	static constexpr arch::field<uint32_t, bool> enablePll(31, 1);
}

namespace bus_multiplier {
	static constexpr arch::field<uint32_t, unsigned int> vgaMultiplier(0, 6);
	static constexpr arch::field<uint32_t, unsigned int> dacMultiplier(8, 6);
}

namespace pll_divisor {
	static constexpr arch::field<uint32_t, unsigned int> m2(0, 6);
	static constexpr arch::field<uint32_t, unsigned int> m1(8, 6);
	static constexpr arch::field<uint32_t, unsigned int> n(16, 6);
}

// ----------------------------------------------------------------------------
// Pipe timing registers.
// ----------------------------------------------------------------------------

namespace regs {
	static constexpr arch::bit_register<uint32_t> htotal(0x60000);
	static constexpr arch::bit_register<uint32_t> hblank(0x60004);
	static constexpr arch::bit_register<uint32_t> hsync(0x60008);
	static constexpr arch::bit_register<uint32_t> vtotal(0x6000C);
	static constexpr arch::bit_register<uint32_t> vblank(0x60010);
	static constexpr arch::bit_register<uint32_t> vsync(0x60014);
	static constexpr arch::bit_register<uint32_t> sourceSize(0x6001C);
}

namespace hvtotal {
	static constexpr arch::field<uint32_t, unsigned int> active(0, 12);
	static constexpr arch::field<uint32_t, unsigned int> total(16, 13);
}

namespace hvblank {
	static constexpr arch::field<uint32_t, unsigned int> start(0, 13);
	static constexpr arch::field<uint32_t, unsigned int> end(16, 13);
}

namespace hvsync {
	static constexpr arch::field<uint32_t, unsigned int> start(0, 13);
	static constexpr arch::field<uint32_t, unsigned int> end(16, 13);
}

namespace source_size {
	static constexpr arch::field<uint32_t, unsigned int> horizontal(16, 12);
	static constexpr arch::field<uint32_t, unsigned int> vertical(0, 12);
}

// ----------------------------------------------------------------------------
// Port registers.
// ----------------------------------------------------------------------------

namespace regs {
	static constexpr arch::bit_register<uint32_t> dacPort(0x61100);
}

namespace dac_port {
	static constexpr arch::field<uint32_t, bool> enableDac(31, 1);
}

// ----------------------------------------------------------------------------
// Pipe registers.
// ----------------------------------------------------------------------------

namespace regs {
	static constexpr arch::bit_register<uint32_t> pipeConfig(0x70008);
}

namespace pipe_config {
	static constexpr arch::field<uint32_t, bool> pipeStatus(30, 1);
	static constexpr arch::field<uint32_t, bool> enablePipe(31, 1);
}

// ----------------------------------------------------------------------------
// Primary plane registers.
// ----------------------------------------------------------------------------

namespace regs {
	static constexpr arch::bit_register<uint32_t> planeControl(0x70180);
	static constexpr arch::scalar_register<uint32_t> planeOffset(0x70184);
	static constexpr arch::scalar_register<uint32_t> planeStride(0x70188);
	static constexpr arch::scalar_register<uint32_t> planeAddress(0x7019C);
}

enum class PrimaryFormat : unsigned int {
	INDEXED = 2,
	BGRX8888 = 6,
	RGBX8888 = 14
};

namespace plane_control {
	static constexpr arch::field<uint32_t, bool> enablePlane(31, 1);
	static constexpr arch::field<uint32_t, PrimaryFormat> pixelFormat(26, 4);
}

namespace vga_control {
	static constexpr arch::field<uint32_t, bool> disableVga(31, 1);
	static constexpr arch::field<uint32_t, unsigned int> centeringMode(30, 2);
}

// ----------------------------------------------------------------------------
// VGA BIOS registers.
// ----------------------------------------------------------------------------

namespace regs {
	static constexpr arch::bit_register<uint32_t> vgaControl(0x71400);
}

