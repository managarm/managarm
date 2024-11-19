#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <riscv/csr.hpp>

namespace eir {

namespace {

void handleException() {
	auto cause = riscv::readCsr<riscv::Csr::scause>();
	auto ip = riscv::readCsr<riscv::Csr::sepc>();
	auto trapValue = riscv::readCsr<riscv::Csr::stval>();
	eir::infoLogger() << "Exception with cause 0x" << frg::hex_fmt{cause}
			<< ", trap value 0x" << frg::hex_fmt{trapValue}
			<< " at IP 0x" << frg::hex_fmt{ip} << frg::endlog;
	while(true)
		;
}

} // namespace

void initProcessorEarly() {
	riscv::writeCsr<riscv::Csr::stvec>(reinterpret_cast<uint64_t>(&handleException));
}

} // namespace eir
