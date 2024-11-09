#include <thor-internal/arch/debug.hpp>
#include <arch/aarch64/mem_space.hpp>
#include <arch/register.hpp>

namespace thor {

constinit FirmwareLogHandler firmwareLogHandler;

void setupDebugging() {
	enableLogHandler(&firmwareLogHandler);
}

void FirmwareLogHandler::sbiCall1(int ext, int func, SbiWord arg0) {
	register SbiWord rExt asm("a7") = ext;
	register SbiWord rFunc asm("a6") = func;
	register SbiWord rArg0 asm("a0") = arg0;
	register SbiWord rArg1 asm("a1");
	asm volatile("ecall" : "+r"(rArg0), "=r"(rArg1) : "r"(rExt), "r"(rFunc));
	if(rArg0)
		__builtin_trap();
}

void FirmwareLogHandler::printChar(char c) {
	// This firmware call is technically depreceated, but is still
	// almost always supported
	sbiCall1(1, 0, c);
}

} // namespace thor
