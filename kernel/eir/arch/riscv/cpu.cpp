#include <dtb.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <riscv/csr.hpp>

namespace eir {

namespace {

void handleException() {
	auto cause = riscv::readCsr<riscv::Csr::scause>();
	auto ip = riscv::readCsr<riscv::Csr::sepc>();
	auto trapValue = riscv::readCsr<riscv::Csr::stval>();
	eir::infoLogger() << "Exception with cause 0x" << frg::hex_fmt{cause} << ", trap value 0x"
	                  << frg::hex_fmt{trapValue} << " at IP 0x" << frg::hex_fmt{ip} << frg::endlog;
	while (true)
		;
}

// All bits need to be enabled to ensure RVA22 compliance
struct RiscVBits {
	bool i, m, a, f, d, c, zicsr, zicntr, ziccif, ziccrse, ziccamoa, zicclsm, za64rs, zihpm,
	    zihintpause, zic64b, zicbom, zicbop, zicboz;
};

} // namespace

void initProcessorEarly() {
	riscv::writeCsr<riscv::Csr::stvec>(reinterpret_cast<uint64_t>(&handleException));
	// Check whether or not our environment is capable of all RVA22 extensions.
	// TODO: If ACPI is enabled, we might not have a device tree. In that case, use the RHCT to get
	// the ISA string.
	if (dtb) {
		DeviceTree dt(physToVirt<void>(dtb));

		// Get the first "/cpus/cpu@..."
		frg::optional<DeviceTreeNode> cpuNode;
		dt.rootNode().discoverSubnodes(
		    [](auto node) { return frg::string_view(node.name()) == "cpus"; },
		    [&](auto cpus) {
			    cpus.discoverSubnodes(
			        [](auto node) { return frg::string_view(node.name()).starts_with("cpu@"); },
			        [&](auto a) { cpuNode = a; }
			    );
		    }
		);
		assert(cpuNode.has_value());

		// Check isa-base
		auto isaBase = cpuNode->findProperty("riscv,isa-base");
		assert(isaBase.has_value());
		if (isaBase.value().asString() != "rv64i") {
			infoLogger() << "eir: This device does not have rv64i base! riscv,isa-base = \""
			             << isaBase.value().asString().value() << "\"" << frg::endlog;
		}

		// Check isa-extensions
		auto isaExtensions = cpuNode->findProperty("riscv,isa-extensions");
		assert(isaExtensions.has_value());
		RiscVBits bits{};
		for (size_t i = 0; isaExtensions.value().asString(i).has_value(); i++) {
			auto extension = isaExtensions.value().asString(i).value();
			if (extension == "i")
				bits.i = true;
			if (extension == "m")
				bits.m = true;
			if (extension == "a")
				bits.a = true;
			if (extension == "f")
				bits.f = true;
			if (extension == "d")
				bits.d = true;
			if (extension == "c")
				bits.c = true;
			if (extension == "zicsr")
				bits.zicsr = true;
			if (extension == "zicntr")
				bits.zicntr = true;
			if (extension == "ziccif")
				bits.ziccif = true;
			if (extension == "ziccrse")
				bits.ziccrse = true;
			if (extension == "ziccamoa")
				bits.ziccamoa = true;
			if (extension == "zicclsm")
				bits.zicclsm = true;
			if (extension == "za64rs")
				bits.za64rs = true;
			if (extension == "zihpm")
				bits.zihpm = true;
			if (extension == "zihintpause")
				bits.zihintpause = true;
			if (extension == "zic64b")
				bits.zic64b = true;
			if (extension == "zicbom")
				bits.zicbom = true;
			if (extension == "zicbop")
				bits.zicbop = true;
			if (extension == "zicboz")
				bits.zicboz = true;
		}
		if (!(bits.i && bits.m && bits.a && bits.f && bits.d && bits.c && bits.zicsr &&
		      bits.zicntr && bits.ziccif && bits.ziccrse && bits.ziccamoa && bits.zicclsm &&
		      bits.za64rs && bits.zihpm && bits.zihintpause && bits.zic64b && bits.zicbom &&
		      bits.zicbop && bits.zicboz)) {
			panicLogger() << "Processor doesn't support all RVA22 extensions!" << frg::endlog;
		}
	}
}

} // namespace eir
