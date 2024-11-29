#include <thor-internal/arch/fp-state.hpp>

namespace thor {

void saveFpRegisters(void *ptr) {
	// clang-format off
	asm volatile (
		     "fsd  f0,  0x0(%0)" "\n"
		"\r" "fsd  f1,  0x8(%0)" "\n"
		"\r" "fsd  f2, 0x10(%0)" "\n"
		"\r" "fsd  f3, 0x18(%0)" "\n"
		"\r" "fsd  f4, 0x20(%0)" "\n"
		"\r" "fsd  f5, 0x28(%0)" "\n"
		"\r" "fsd  f6, 0x30(%0)" "\n"
		"\r" "fsd  f7, 0x38(%0)" "\n"
		"\r" "fsd  f8, 0x40(%0)" "\n"
		"\r" "fsd  f9, 0x48(%0)" "\n"
		"\r" "fsd f10, 0x50(%0)" "\n"
		"\r" "fsd f11, 0x58(%0)" "\n"
		"\r" "fsd f12, 0x60(%0)" "\n"
		"\r" "fsd f13, 0x68(%0)" "\n"
		"\r" "fsd f14, 0x70(%0)" "\n"
		"\r" "fsd f15, 0x78(%0)" "\n"
		"\r" "fsd f16, 0x80(%0)" "\n"
		"\r" "fsd f17, 0x88(%0)" "\n"
		"\r" "fsd f18, 0x90(%0)" "\n"
		"\r" "fsd f19, 0x98(%0)" "\n"
		"\r" "fsd f20, 0xA0(%0)" "\n"
		"\r" "fsd f21, 0xA8(%0)" "\n"
		"\r" "fsd f22, 0xB0(%0)" "\n"
		"\r" "fsd f23, 0xB8(%0)" "\n"
		"\r" "fsd f24, 0xC0(%0)" "\n"
		"\r" "fsd f25, 0xC8(%0)" "\n"
		"\r" "fsd f26, 0xD0(%0)" "\n"
		"\r" "fsd f27, 0xD8(%0)" "\n"
		"\r" "fsd f28, 0xE0(%0)" "\n"
		"\r" "fsd f29, 0xE8(%0)" "\n"
		"\r" "fsd f30, 0xF0(%0)" "\n"
		"\r" "fsd f31, 0xF8(%0)"
		: : "r"(ptr) : "memory"
	);
	// clang-format on
}

void restoreFpRegisters(void *ptr) {
	// clang-format off
	asm volatile (
		     "fld  f0,  0x0(%0)" "\n"
		"\r" "fld  f1,  0x8(%0)" "\n"
		"\r" "fld  f2, 0x10(%0)" "\n"
		"\r" "fld  f3, 0x18(%0)" "\n"
		"\r" "fld  f4, 0x20(%0)" "\n"
		"\r" "fld  f5, 0x28(%0)" "\n"
		"\r" "fld  f6, 0x30(%0)" "\n"
		"\r" "fld  f7, 0x38(%0)" "\n"
		"\r" "fld  f8, 0x40(%0)" "\n"
		"\r" "fld  f9, 0x48(%0)" "\n"
		"\r" "fld f10, 0x50(%0)" "\n"
		"\r" "fld f11, 0x58(%0)" "\n"
		"\r" "fld f12, 0x60(%0)" "\n"
		"\r" "fld f13, 0x68(%0)" "\n"
		"\r" "fld f14, 0x70(%0)" "\n"
		"\r" "fld f15, 0x78(%0)" "\n"
		"\r" "fld f16, 0x80(%0)" "\n"
		"\r" "fld f17, 0x88(%0)" "\n"
		"\r" "fld f18, 0x90(%0)" "\n"
		"\r" "fld f19, 0x98(%0)" "\n"
		"\r" "fld f20, 0xA0(%0)" "\n"
		"\r" "fld f21, 0xA8(%0)" "\n"
		"\r" "fld f22, 0xB0(%0)" "\n"
		"\r" "fld f23, 0xB8(%0)" "\n"
		"\r" "fld f24, 0xC0(%0)" "\n"
		"\r" "fld f25, 0xC8(%0)" "\n"
		"\r" "fld f26, 0xD0(%0)" "\n"
		"\r" "fld f27, 0xD8(%0)" "\n"
		"\r" "fld f28, 0xE0(%0)" "\n"
		"\r" "fld f29, 0xE8(%0)" "\n"
		"\r" "fld f30, 0xF0(%0)" "\n"
		"\r" "fld f31, 0xF8(%0)"
		: : "r"(ptr) : "memory"
	);
	// clang-format on
}

} // namespace thor
