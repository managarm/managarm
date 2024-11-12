#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>

namespace eir {

void enterKernel() {
	infoLogger() << "enterKernel() is unimplemented on RISC-V" << frg::endlog;
	__builtin_trap();
}

} // namespace eir
