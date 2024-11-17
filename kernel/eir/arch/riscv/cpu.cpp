#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <riscv/csr.hpp>

namespace eir {

namespace {

void handleException() {
	auto cause = readCsr<Csr::scause>();
	auto ip = readCsr<Csr::sepc>();
	auto trapValue = readCsr<Csr::stval>();
	eir::infoLogger() << "Exception with cause 0x" << frg::hex_fmt{cause}
			<< ", trap value 0x" << frg::hex_fmt{trapValue}
			<< " at IP 0x" << frg::hex_fmt{ip} << frg::endlog;
	while(true)
		;
}

} // namespace

void initProcessorEarly() {
	writeCsr<Csr::stvec>(reinterpret_cast<uint64_t>(&handleException));
}

} // namespace eir
