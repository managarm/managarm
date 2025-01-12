#include <frg/dyn_array.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/dtb/irq.hpp>
#include <thor-internal/main.hpp>

namespace thor {

namespace {

constexpr bool logMaskUnmask = false;
constexpr bool logClaim = false;

const frg::array<frg::string_view, 1> aplicCompatible = {"riscv,aplic"};
const frg::array<frg::string_view, 1> intcCompatible = {"riscv,cpu-intc"};
const frg::array<frg::string_view, 1> cpuCompatible = {"riscv"};

constexpr arch::scalar_register<uint32_t> aplicDomaincfgRegister{0};

// Per interrupt-source registers.
constexpr auto aplicSourcecfgRegister(size_t n) {
	assert(n > 0);
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(4 * n)};
}
constexpr auto aplicTargetRegister(size_t n) {
	assert(n > 0);
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x3000 + 4 * n)};
}

// Per interrupt-source registers (one bit per source).
constexpr auto aplicSetipRegister(size_t n) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x1c00 + 4 * n)};
}
constexpr auto aplicInRegister(size_t n) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x1d00 + 4 * n)};
}
constexpr auto aplicSetieRegister(size_t n) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x1e00 + 4 * n)};
}
constexpr auto aplicClrieRegister(size_t n) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x1f00 + 4 * n)};
}

// Per hart registers.
constexpr auto aplicIdeliveryRegister(size_t idx) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x4000 + 32 * idx)};
}
constexpr auto aplicIthresholdRegister(size_t idx) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x4000 + 32 * idx + 0x8)};
}
constexpr auto aplicTopiRegister(size_t idx) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x4000 + 32 * idx + 0x18)};
}
constexpr auto aplicClaimiRegister(size_t idx) {
	return arch::scalar_register<uint32_t>{static_cast<ptrdiff_t>(0x4000 + 32 * idx + 0x1c)};
}

struct Aplic : dt::IrqController {
	static frg::string<KernelAlloc> buildName(Aplic *aplic, size_t idx) {
		return frg::string<KernelAlloc>{*kernelAlloc, "aplic@"}
		       + frg::to_allocated_string(*kernelAlloc, aplic->base_, 16)
		       + frg::string<KernelAlloc>{*kernelAlloc, ":"}
		       + frg::to_allocated_string(*kernelAlloc, idx);
	}

	struct Irq final : IrqPin {
		Irq(Aplic *aplic, size_t idx) : IrqPin{buildName(aplic, idx)}, aplic_{aplic}, idx_{idx} {
			assert(idx_);
		}

		Irq(const Irq &) = delete;
		Irq &operator=(const Irq &) = delete;

		IrqStrategy program(TriggerMode trigger, Polarity polarity) override {
			uint32_t mode; // Source mode as defined by the APLIC spec.
			IrqStrategy strategy;
			if (trigger == TriggerMode::edge) {
				if (polarity == Polarity::high) {
					mode = 4;
				} else {
					assert(polarity == Polarity::low);
					mode = 5;
				}
				strategy = IrqStrategy::justEoi;
			} else {
				assert(trigger == TriggerMode::level);
				if (polarity == Polarity::high) {
					mode = 6;
				} else {
					assert(polarity == Polarity::low);
					mode = 7;
				}
				strategy = IrqStrategy::maskThenEoi;
			}

			// Set the source mode, ensure that this source mode is supported.
			infoLogger() << "Programming APLIC source " << name() << " to source mode " << mode
			             << frg::endlog;
			aplic_->space_.store(aplicSourcecfgRegister(idx_), mode);
			if (aplic_->space_.load(aplicSourcecfgRegister(idx_)) != mode)
				panicLogger() << "APLIC source " << name() << " does not support source mode "
				              << mode << frg::endlog;

			// Program the source to target the BSP, set priority to 1.
			aplic_->space_.store(aplicTargetRegister(idx_), (aplic_->bspIdx_ << 18) | 1);

			unmask();

			return strategy;
		}

		// Mostly useful for debugging.
		int checkInput() {
			auto in = aplic_->space_.load(aplicInRegister(idx_ >> 5));
			return static_cast<bool>(in & (UINT32_C(1) << (idx_ & 0x1F)));
		}

		// Mostly useful for debugging.
		int checkPending() {
			auto pending = aplic_->space_.load(aplicSetipRegister(idx_ >> 5));
			return static_cast<bool>(pending & (UINT32_C(1) << (idx_ & 0x1F)));
		}

		void mask() override {
			if (logMaskUnmask)
				infoLogger() << "APLIC: Masking source " << name() << frg::endlog;
			uint32_t bits = UINT32_C(1) << (idx_ & 0x1F);
			aplic_->space_.store(aplicClrieRegister(idx_ >> 5), bits);
		}

		void unmask() override {
			if (logMaskUnmask)
				infoLogger() << "APLIC: Unmasking source " << name() << frg::endlog;
			uint32_t bits = UINT32_C(1) << (idx_ & 0x1F);
			aplic_->space_.store(aplicSetieRegister(idx_ >> 5), bits);
		}

		void sendEoi() override {
			// The APLIC does not require EOIs.
		}

	private:
		Aplic *aplic_;
		size_t idx_;
	};

	Aplic(PhysicalAddr base, size_t size, size_t numIrqs, size_t bspIdx)
	: base_{base},
	  size_{size},
	  bspIdx_{bspIdx} {
		auto ptr = KernelVirtualMemory::global().allocate(size);
		for (size_t i = 0; i < size; i += kPageSize) {
			KernelPageSpace::global().mapSingle4k(
			    reinterpret_cast<uintptr_t>(ptr) + i,
			    base + i,
			    page_access::write,
			    CachingMode::mmio
			);
		}
		space_ = arch::mem_space{ptr};

		irqs_ = {numIrqs, *kernelAlloc};
		irqs_[0] = nullptr;
		for (size_t i = 1; i < numIrqs; ++i)
			irqs_[i] = frg::construct<Irq>(*kernelAlloc, this, i);

		// This should set BE = 0 and IE = 0.
		space_.store(aplicDomaincfgRegister, 0);

		// Panic if the APLIC is hardwired to BE = 1.
		auto domaincfg = space_.load(aplicDomaincfgRegister);
		if ((domaincfg >> 24) != 0x80)
			panicLogger() << "APLIC is big-endian" << frg::endlog;

		// Disable all sources.
		for (size_t i = 1; i < numIrqs; ++i)
			space_.store(aplicSourcecfgRegister(i), 0);

		space_.store(aplicIdeliveryRegister(bspIdx_), 1);
		space_.store(aplicIthresholdRegister(bspIdx_), 0);

		// Set IE.
		space_.store(aplicDomaincfgRegister, 0x100);
	}

	Aplic(const Aplic &) = delete;
	Aplic &operator=(const Aplic &) = delete;

	IrqPin *getIrq(size_t idx) {
		assert(idx);
		assert(idx < irqs_.size());
		return irqs_[idx];
	}

	IrqPin *resolveDtIrq(dtb::Cells irqSpecifier) override {
		if (irqSpecifier.numCells() != 2)
			panicLogger() << "APLIC #interrupt-cells should be 2" << frg::endlog;
		uint32_t idx;
		uint32_t flags;
		if (!irqSpecifier.readSlice(idx, 0, 1))
			panicLogger() << "Failed to read APLIC interrupt index" << frg::endlog;
		if (!irqSpecifier.readSlice(flags, 1, 1))
			panicLogger() << "Failed to read APLIC interrupt flags" << frg::endlog;

		IrqConfiguration cfg;
		switch (flags) {
			case 1:
				cfg = {TriggerMode::edge, Polarity::high};
				break;
			case 2:
				cfg = {TriggerMode::edge, Polarity::low};
				break;
			case 4:
				cfg = {TriggerMode::level, Polarity::high};
				break;
			case 8:
				cfg = {TriggerMode::level, Polarity::low};
				break;
			default:
				panicLogger() << "Unexpected flags 0x" << frg::hex_fmt{flags}
				              << " in APLIC interrupt specifier" << frg::endlog;
		}

		auto pin = getIrq(idx);
		pin->configure(cfg);
		return pin;
	}

	uint32_t claim(size_t idx) {
		auto claimi = space_.load(aplicClaimiRegister(idx));
		auto srcIdx = claimi >> 16;
		if (logClaim)
			infoLogger() << "APLIC claim returns source " << srcIdx << frg::endlog;
		return srcIdx;
	}

private:
	PhysicalAddr base_;
	size_t size_;
	arch::mem_space space_;
	frg::dyn_array<Irq *, KernelAlloc> irqs_{*kernelAlloc};
	// TODO: The current implementation routes all IRQs to the BSP.
	//       We should allow routing of IRQs to other harts as well.
	size_t bspIdx_;
};

void enumerateAplic(DeviceTreeNode *aplicNode) {
	infoLogger() << "thor: Found APLIC " << aplicNode->path() << frg::endlog; // FIXME

	const auto &reg = aplicNode->reg();
	if (reg.size() != 1)
		panicLogger() << "thor: Expect exactly one 'reg' entry for APLICs" << frg::endlog;

	size_t bspIdx{~size_t{0}};
	size_t i = 0;
	bool success = dt::walkInterruptsExtended(
	    [&](DeviceTreeNode *intcNode, dtb::Cells intcIrq) {
		    if (!intcNode->isCompatible(intcCompatible))
			    panicLogger() << "Expected interrupt parent of APLIC to be cpu-intc device"
			                  << frg::endlog;

		    // Find the CPU corresponding to the APLIC hard index based on the cpu-intc node.
		    auto cpuNode = intcNode->parent();
		    if (!cpuNode || !cpuNode->isCompatible(cpuCompatible))
			    panicLogger() << "Expected parent of cpu-intc device to be CPU" << frg::endlog;
		    auto hartId = cpuNode->reg().front().addr;

		    // Get the IRQ index at the parent cpu-intc.
		    uint32_t intcIdx;
		    if (!intcIrq.read(intcIdx))
			    panicLogger() << "Failed to read cpu-intc interrupt index" << frg::endlog;
		    infoLogger() << "    Hart index " << i << " connected to hart ID " << hartId
		                 << ", interrupt " << intcIdx << frg::endlog;

		    if (hartId == getCpuData()->hartId && intcIdx == riscv::interrupts::sei)
			    bspIdx = i;
		    ++i;
	    },
	    aplicNode
	);
	if (!success)
		panicLogger() << "Failed to walk interrupts of " << aplicNode->path() << frg::endlog;

	if (bspIdx == ~size_t{0}) {
		infoLogger() << "    Failed to determine APLIC hart index of BSP" << frg::endlog;
		return;
	}
	infoLogger() << "    Hard index " << bspIdx << " corresponds to BSP S-mode external interrupt"
	             << frg::endlog;

	auto numSourcesProp = aplicNode->dtNode().findProperty("riscv,num-sources");
	if (!numSourcesProp)
		panicLogger() << "thor: APLIC has no riscv,num-sources property" << frg::endlog;
	uint32_t numSources;
	if (!numSourcesProp->access().readCells(numSources, 1))
		panicLogger() << "thor: Failed to read riscv,num-sources from APLIC" << frg::endlog;

	auto aplic =
	    frg::construct<Aplic>(*kernelAlloc, reg.front().addr, reg.front().size, numSources, bspIdx);
	aplicNode->associateIrqController(aplic);

	auto *ourExternalIrq = &riscvExternalIrq.get();
	ourExternalIrq->type = ExternalIrqType::aplic;
	ourExternalIrq->controller = aplic;
	ourExternalIrq->context = bspIdx;
}

initgraph::Task initPlic{
    &globalInitEngine,
    "riscv.init-aplic",
    initgraph::Requires{getDeviceTreeParsedStage()},
    initgraph::Entails{getTaskingAvailableStage()},
    [] {
	    getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
		    if (node->isCompatible(aplicCompatible))
			    enumerateAplic(node);
		    return false;
	    });
    }
};

} // namespace

IrqPin *claimAplicIrq() {
	auto *ourExternalIrq = &riscvExternalIrq.get();
	assert(ourExternalIrq->type == ExternalIrqType::aplic);
	assert(ourExternalIrq->controller);
	auto *aplic = static_cast<Aplic *>(ourExternalIrq->controller);
	auto idx = aplic->claim(ourExternalIrq->context);
	if (!idx)
		return nullptr;
	return aplic->getIrq(idx);
}

} // namespace thor
