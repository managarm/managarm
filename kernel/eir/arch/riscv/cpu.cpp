#include <algorithm>
#include <dtb.hpp>
#include <eir-internal/arch.hpp>
#include <eir-internal/debug.hpp>
#include <eir-internal/main.hpp>
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
enum RiscVBits {
	i,
	m,
	a,
	f,
	d,
	c,
	zicsr,
	zicntr,
	ziccif,
	ziccrse,
	ziccamoa,
	zicclsm,
	za64rs,
	zihpm,
	zihintpause,
	zic64b,
	zicbom,
	zicbop,
	zicboz,
};

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
	bool bits[19]{};
	for (size_t i = 0; isaExtensions.value().asString(i).has_value(); i++) {
		auto extension = isaExtensions.value().asString(i).value();
		infoLogger() << "eir: Have extension " << extension << frg::endlog;
		if (extension == "i")
			bits[RiscVBits::i] = true;
		if (extension == "m")
			bits[RiscVBits::m] = true;
		if (extension == "a")
			bits[RiscVBits::a] = true;
		if (extension == "f")
			bits[RiscVBits::f] = true;
		if (extension == "d")
			bits[RiscVBits::d] = true;
		if (extension == "c")
			bits[RiscVBits::c] = true;
		if (extension == "zicsr")
			bits[RiscVBits::zicsr] = true;
		if (extension == "zicntr")
			bits[RiscVBits::zicntr] = true;
		if (extension == "ziccif")
			bits[RiscVBits::ziccif] = true;
		if (extension == "ziccrse")
			bits[RiscVBits::ziccrse] = true;
		if (extension == "ziccamoa")
			bits[RiscVBits::ziccamoa] = true;
		if (extension == "zicclsm")
			bits[RiscVBits::zicclsm] = true;
		if (extension == "za64rs")
			bits[RiscVBits::za64rs] = true;
		if (extension == "zihpm")
			bits[RiscVBits::zihpm] = true;
		if (extension == "zihintpause")
			bits[RiscVBits::zihintpause] = true;
		if (extension == "zic64b")
			bits[RiscVBits::zic64b] = true;
		if (extension == "zicbom")
			bits[RiscVBits::zicbom] = true;
		if (extension == "zicbop")
			bits[RiscVBits::zicbop] = true;
		if (extension == "zicboz")
			bits[RiscVBits::zicboz] = true;
	}

	// If not all bits are set, some kernel functionality may be impacted.
	if (!std::ranges::all_of(bits, [](auto a) { return a; })) {
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
		if (mmuTypeStr != "riscv,sv39" && mmuTypeStr != "riscv,sv48" && mmuTypeStr != "riscv,sv57")
			panicLogger() << "Processor does not support either Sv39, Sv48 or Sv57!" << frg::endlog;
	} else {
		panicLogger() << "Unable to boot without a device tree!" << frg::endlog;
	}
}

} // namespace eir
