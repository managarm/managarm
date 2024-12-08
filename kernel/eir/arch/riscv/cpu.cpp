#include <algorithm>
#include <dtb.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/arch/riscv.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/main.hpp>
#include <riscv/csr.hpp>

namespace eir {

constinit RiscvHartCaps riscvHartCaps;

namespace {

// All bits need to be enabled to ensure RVA22 compliance
constexpr std::array rva22Mandatory{
    RiscvExtension::i,       RiscvExtension::m,        RiscvExtension::a,
    RiscvExtension::f,       RiscvExtension::d,        RiscvExtension::c,
    RiscvExtension::zicsr,   RiscvExtension::zicntr,   RiscvExtension::ziccif,
    RiscvExtension::ziccrse, RiscvExtension::ziccamoa, RiscvExtension::zicclsm,
    RiscvExtension::za64rs,  RiscvExtension::zihpm,    RiscvExtension::zihintpause,
    RiscvExtension::zic64b,  RiscvExtension::zicbom,   RiscvExtension::zicbop,
    RiscvExtension::zicboz,
};

void handleException() {
	auto cause = riscv::readCsr<riscv::Csr::scause>();
	auto ip = riscv::readCsr<riscv::Csr::sepc>();
	auto trapValue = riscv::readCsr<riscv::Csr::stval>();
	eir::infoLogger() << "Exception with cause 0x" << frg::hex_fmt{cause} << ", trap value 0x"
	                  << frg::hex_fmt{trapValue} << " at IP 0x" << frg::hex_fmt{ip} << frg::endlog;
	while (true)
		;
}

// Handle "isa-base" and "isa-extensions"
void checkIsaBaseExtensions(DeviceTreeNode cpuNode) {
	// Check isa-base.
	auto isaBase = cpuNode.findProperty("riscv,isa-base");
	if (!isaBase.has_value()) {
		infoLogger() << "eir: No isa-base found" << frg::endlog;
		return;
	}
	if (isaBase.value().asString() != "rv64i") {
		panicLogger() << "eir: This device does not have rv64i base! riscv,isa-base = "
		              << "\"" << isaBase.value().asString().value() << "\"" << frg::endlog;
	}

	// Check isa-extensions.
	auto isaExtensions = cpuNode.findProperty("riscv,isa-extensions");
	if (!isaExtensions.has_value()) {
		infoLogger() << "eir: No isa-extensions found" << frg::endlog;
		return;
	}
	for (size_t i = 0; isaExtensions.value().asString(i).has_value(); i++) {
		auto s = isaExtensions.value().asString(i).value();
		auto ext = parseRiscvExtension(s);
		if (ext != RiscvExtension::invalid) {
			riscvHartCaps.setExtension(ext);
			infoLogger() << "eir: Have extension " << s << frg::endlog;
		} else {
			infoLogger() << "eir: riscv,isa-extensions reports unknown extension " << s
			             << frg::endlog;
		}
	}

	// If not all bits are set, some kernel functionality may be impacted.
	if (!std::ranges::all_of(rva22Mandatory, [](auto ext) {
		    return riscvHartCaps.hasExtension(ext);
	    })) {
		infoLogger() << "Processor does not support all mandatory RVA22 extensions!" << frg::endlog;
	}
}

} // namespace

void initProcessorEarly() {
	riscv::writeCsr<riscv::Csr::stvec>(reinterpret_cast<uint64_t>(&handleException));

	// Check whether or not our environment is capable of all RVA22 extensions.
	// TODO: If ACPI is enabled, we might not have a device tree. In that case, use the RHCT to get
	// the ISA string.
	if (eirDtbPtr) {
		DeviceTree dt(eirDtbPtr);

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

		checkIsaBaseExtensions(*cpuNode);

		// Make sure at least Sv39 is available.
		// TODO: Technically, "mmu-type" is not required to be present. If it is not present,
		//       we could auto-detect the MMU type of the BSP by trying to write satp.
		//       satp is not unchanged on writes that would result in unsupported modes.
		auto mmuType = cpuNode->findProperty("mmu-type");
		if (!mmuType)
			panicLogger() << "mmu-type property is missing" << frg::endlog;
		auto mmuTypeStr = mmuType->asString();
		if (mmuTypeStr == "riscv,sv39") {
			riscvConfig.numPtLevels = 3;
		} else if (mmuTypeStr == "riscv,sv48" || mmuTypeStr == "riscv,sv57") {
			riscvConfig.numPtLevels = 4; // Use Sv48 in both cases.
		} else {
			panicLogger() << "Processor does not support either Sv39, Sv48 or Sv57!" << frg::endlog;
		}
		infoLogger() << "eir: Supported mmu-type is " << *mmuTypeStr << frg::endlog;
		infoLogger() << "eir: Using " << riscvConfig.numPtLevels << " levels of page tables"
		             << frg::endlog;
	} else {
		panicLogger() << "Unable to boot without a device tree!" << frg::endlog;
	}
}

} // namespace eir
