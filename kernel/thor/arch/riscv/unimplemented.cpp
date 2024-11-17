#include <thor-internal/arch/unimplemented.hpp>
#include <thor-internal/debug.hpp>

void thor::unimplementedOnRiscv(std::source_location loc) {
	panicLogger() << "Function " << loc.function_name()
	              << " is unimplemented on RISC-V"
	                 ", at "
	              << loc.file_name() << ":" << loc.line() << frg::endlog;
	__builtin_trap(); // Stop Clang from complaining about [[noreturn]].
}
