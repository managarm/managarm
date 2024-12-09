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

// Handle "riscv,isa".
bool checkIsa(DeviceTreeNode cpuNode) {
	auto isa = cpuNode.findProperty("riscv,isa");
	if (!isa.has_value())
		return false;
	auto s = *isa->asString();

	if (!s.starts_with("rv64"))
		panicLogger() << "eir: riscv,isa does not match rv64" << frg::endlog;

	size_t n = 4; // Skip rv64.
	while (n < s.size()) {
		auto p = n;

		// Underscores are used to separate extensions.
		if (s[n] == '_') {
			++n;
			continue;
		}

		// Advance n until we have see the entire extension.
		// s, z and x indicate multi-character extensions.
		// All other extensions are single character.
		if (s[n] == 's' || s[n] == 'z' || s[n] == 'x') {
			// Consume the s or z char.
			++n;
			// Consume additional chars.
			while (n < s.size() && s[n] >= 'a' && s[n] <= 'z')
				++n;
		} else {
			// Consume a single char.
			++n;
		}

		auto extStr = s.sub_string(p, n - p);
		auto ext = parseRiscvExtension(extStr);
		if (ext != RiscvExtension::invalid) {
			riscvHartCaps.setExtension(ext);
			infoLogger() << "eir: Have extension " << extStr << frg::endlog;

			// For the riscv,isa property, the i extension implies zicntr_zicsr_zifencei_zihpm.
			// Note that this does not apply to riscv,isa-extensions.
			if (ext == RiscvExtension::i) {
				riscvHartCaps.setExtension(RiscvExtension::zicntr);
				riscvHartCaps.setExtension(RiscvExtension::zicsr);
				riscvHartCaps.setExtension(RiscvExtension::zifencei);
				riscvHartCaps.setExtension(RiscvExtension::zihpm);
			}
		} else {
			infoLogger() << "eir: riscv,isa reports unknown extension " << extStr << frg::endlog;
		}
	}

	return true;
}

// Handle "riscv,isa-base" and "riscv,isa-extensions".
bool checkIsaBaseExtensions(DeviceTreeNode cpuNode) {
	// Check isa-base.
	auto isaBase = cpuNode.findProperty("riscv,isa-base");
	if (!isaBase.has_value())
		return false;
	if (isaBase.value().asString() != "rv64i") {
		panicLogger() << "eir: This device does not have rv64i base! riscv,isa-base = "
		              << "\"" << isaBase.value().asString().value() << "\"" << frg::endlog;
	}

	// Check isa-extensions.
	auto isaExtensions = cpuNode.findProperty("riscv,isa-extensions");
	if (!isaExtensions.has_value()) {
		infoLogger() << "eir: No riscv,isa-extensions found" << frg::endlog;
		return false;
	}
	for (size_t i = 0; isaExtensions.value().asString(i).has_value(); i++) {
		auto extStr = isaExtensions.value().asString(i).value();
		auto ext = parseRiscvExtension(extStr);
		if (ext != RiscvExtension::invalid) {
			riscvHartCaps.setExtension(ext);
			infoLogger() << "eir: Have extension " << extStr << frg::endlog;
		} else {
			infoLogger() << "eir: riscv,isa-extensions reports unknown extension " << extStr
			             << frg::endlog;
		}
	}

	return true;
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

		// riscv,isa-base + riscv,isa-extensions should be preferred over riscv,isa.
		if (!checkIsaBaseExtensions(*cpuNode) && !checkIsa(*cpuNode))
			panicLogger() << "Both riscv,base and riscv,isa are missing from DT" << frg::endlog;

		// If not all bits are set, some kernel functionality may be impacted.
		if (!std::ranges::all_of(rva22Mandatory, [](auto ext) {
			    return riscvHartCaps.hasExtension(ext);
		    })) {
			infoLogger() << "Processor does not support all mandatory RVA22 extensions!"
			             << frg::endlog;
		}

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
