#include <frg/dyn_array.hpp>
#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/arch/trap.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/dtb/irq.hpp>
#include <thor-internal/main.hpp>

namespace thor {

namespace {

const frg::array<frg::string_view, 1> intcCompatible = {"riscv,cpu-intc"};
const frg::array<frg::string_view, 1> cpuCompatible = {"riscv"};

// ----------------------------------------------------------------------------
// IMSIC
// ----------------------------------------------------------------------------

namespace indirect {
constexpr uint64_t edelivery = 0x70;
constexpr uint64_t ethreshold = 0x72;
constexpr uint64_t eie0 = 0xC0;
}; // namespace indirect

uint64_t readIndirect(uint64_t sel) {
	riscv::writeCsr<riscv::Csr::siselect>(sel);
	return riscv::readCsr<riscv::Csr::sireg>();
}
void writeIndirect(uint64_t sel, uint64_t v) {
	riscv::writeCsr<riscv::Csr::siselect>(sel);
	riscv::writeCsr<riscv::Csr::sireg>(v);
}

const frg::array<frg::string_view, 1> imsciCompatible = {"riscv,imsics"};

struct ImsicContext;

struct Imsic {
	// TODO: Store a dyn_array of all contexts instead of just the BSP's context.
	ImsicContext *bspContext{nullptr};
};

// Per-CPU IMSIC context.
struct ImsicContext {
	uint32_t hartIndex{~UINT32_C(0)};
	frg::dyn_array<IrqPin *, KernelAlloc> irqs{*kernelAlloc};
};

// Only written before APs are booted (no locks needed).
frg::manual_box<frg::hash_map<uint32_t, Imsic *, frg::hash<uint32_t>, KernelAlloc>> phandleToImsic;

void enumerateImsic(DeviceTreeNode *imsicNode) {
	infoLogger() << "thor: Found IMSIC " << imsicNode->path() << frg::endlog;

	size_t bspIdx{~size_t{0}};
	size_t i = 0;
	bool success = dt::walkInterruptsExtended(
	    [&](DeviceTreeNode *intcNode, dtb::Cells intcIrq) {
		    if (!intcNode->isCompatible(intcCompatible))
			    panicLogger() << "Expected interrupt parent of IMSIC to be cpu-intc device"
			                  << frg::endlog;

		    // Find the CPU corresponding to the IMSIC hard index based on the cpu-intc node.
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
	    imsicNode
	);
	if (!success)
		panicLogger() << "Failed to walk interrupts of " << imsicNode->path() << frg::endlog;

	if (bspIdx == ~size_t{0}) {
		infoLogger() << "    Failed to determine IMSIC hart index of BSP" << frg::endlog;
		return;
	}
	infoLogger() << "    Hard index " << bspIdx << " corresponds to BSP S-mode external interrupt"
	             << frg::endlog;

	auto numIdsProp = imsicNode->dtNode().findProperty("riscv,num-ids");
	if (!numIdsProp)
		panicLogger() << "thor: IMSIC has no riscv,num-ids property" << frg::endlog;
	uint32_t numIds;
	if (!numIdsProp->access().readCells(numIds, 1))
		panicLogger() << "thor: Failed to read riscv,num-ids from IMSIC" << frg::endlog;

	// Unmask all IRQs at the IMSIC level (then can be masked at the APLIC level).
	writeIndirect(indirect::ethreshold, 0);
	for (uint32_t i = 0; i < numIds; i += 64) {
		// Note: In 64-bit S-mode, only even eieN registers exist.
		writeIndirect(indirect::eie0 + 2 * (i / 64), ~UINT64_C(0));
	}
	// Enable IMSIC interrupt delivery (not APLIC delivery mode).
	writeIndirect(indirect::edelivery, 1);
	if (readIndirect(indirect::edelivery) != 1)
		panicLogger() << "thor: Failed to enable IMSIC interrupt delivery" << frg::endlog;

	auto *imsic = frg::construct<Imsic>(*kernelAlloc);

	auto *bspContext = frg::construct<ImsicContext>(*kernelAlloc);
	bspContext->hartIndex = bspIdx;
	bspContext->irqs = {numIds, *kernelAlloc};
	imsic->bspContext = bspContext;

	phandleToImsic->insert(imsicNode->phandle(), imsic);

	auto *ourExternalIrq = &riscvExternalIrq.get();
	ourExternalIrq->type = ExternalIrqType::imsic;
	ourExternalIrq->controller = bspContext;
}

// ----------------------------------------------------------------------------
// APLIC
// ----------------------------------------------------------------------------

constexpr bool logMaskUnmask = false;
constexpr bool logClaim = false;

const frg::array<frg::string_view, 1> aplicCompatible = {"riscv,aplic"};

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
[[maybe_unused]] constexpr auto aplicTopiRegister(size_t idx) {
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
				strategy = irq_strategy::maskable;
			} else {
				assert(trigger == TriggerMode::level);
				if (polarity == Polarity::high) {
					mode = 6;
				} else {
					assert(polarity == Polarity::low);
					mode = 7;
				}
				strategy = irq_strategy::maskable | irq_strategy::maskInService;
			}

			// Set the source mode, ensure that this source mode is supported.
			infoLogger() << "Programming APLIC source " << name() << " to source mode " << mode
			             << frg::endlog;
			aplic_->space_.store(aplicSourcecfgRegister(idx_), mode);
			if (aplic_->space_.load(aplicSourcecfgRegister(idx_)) != mode)
				panicLogger() << "APLIC source " << name() << " does not support source mode "
				              << mode << frg::endlog;

			if (aplic_->imsic_) {
				auto *ctx = aplic_->imsic_->bspContext;
				// TODO: Fix this limitation by properly allocating IMSIC interrupts.
				if (idx_ >= ctx->irqs.size())
					panicLogger()
					    << "thor: Cannot identity route APLIC interrupt to IMSIC interrupt " << idx_
					    << frg::endlog;
				ctx->irqs[idx_] = this;
				aplic_->space_.store(aplicTargetRegister(idx_), (ctx->hartIndex << 18) | idx_);
			} else {
				// Program the source to target the BSP, set priority to 1.
				aplic_->space_.store(aplicTargetRegister(idx_), (aplic_->bspIdx_ << 18) | 1);
			}

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

		void endOfInterrupt() override {
			// The APLIC does not require EOIs.
		}

	private:
		Aplic *aplic_;
		size_t idx_;
	};

	Aplic(PhysicalAddr base, size_t size, size_t numIrqs, Imsic *imsic, size_t bspIdx)
	: base_{base},
	  size_{size},
	  imsic_{imsic},
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

		// TODO: Move this to a per-CPU AplicContext class.
		if (!imsic_) {
			space_.store(aplicIdeliveryRegister(bspIdx_), 1);
			space_.store(aplicIthresholdRegister(bspIdx_), 0);
		}

		// Set IE (+ DM if routing as MSIs).
		space_.store(aplicDomaincfgRegister, 0x100 | ((imsic_ ? 1 : 0) << 2));
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

	// TODO: Move this to a per-CPU AplicContext class.
	uint32_t claim(size_t idx) {
		assert(!imsic_); // claim() is only valid for direct routing.

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
	Imsic *imsic_{nullptr};
	frg::dyn_array<Irq *, KernelAlloc> irqs_{*kernelAlloc};
	// TODO: The current implementation routes all IRQs to the BSP.
	//       We should allow routing of IRQs to other harts as well.
	size_t bspIdx_; // Only relevant in direct routing mode.
};

void enumerateAplic(DeviceTreeNode *aplicNode) {
	infoLogger() << "thor: Found APLIC " << aplicNode->path() << frg::endlog;

	const auto &reg = aplicNode->reg();
	if (reg.size() != 1)
		panicLogger() << "thor: Expect exactly one 'reg' entry for APLICs" << frg::endlog;

	auto numSourcesProp = aplicNode->dtNode().findProperty("riscv,num-sources");
	if (!numSourcesProp)
		panicLogger() << "thor: APLIC has no riscv,num-sources property" << frg::endlog;
	uint32_t numSources;
	if (!numSourcesProp->access().readCells(numSources, 1))
		panicLogger() << "thor: Failed to read riscv,num-sources from APLIC" << frg::endlog;

	auto *ourExternalIrq = &riscvExternalIrq.get();
	if (ourExternalIrq->type == ExternalIrqType::imsic) {
		auto msiParentProp = aplicNode->dtNode().findProperty("msi-parent");
		if (!msiParentProp)
			panicLogger() << "thor: APLIC has no msi-parent property" << frg::endlog;
		uint32_t msiParent;
		if (!msiParentProp->access().readCells(msiParent, 1))
			panicLogger() << "thor: Failed to read msi-parent from APLIC" << frg::endlog;

		auto imsicIt = phandleToImsic->find(msiParent);
		if (imsicIt == phandleToImsic->end()) {
			infoLogger() << "thor: APLIC is attached to unknown IMSIC" << frg::endlog;
			return;
		}
		auto *imsic = imsicIt->get<1>();

		infoLogger() << "thor: APLIC is attached to BSP S-mode IMISC" << frg::endlog;

		auto aplic = frg::construct<Aplic>(
		    *kernelAlloc, reg.front().addr, reg.front().size, numSources, imsic, ~size_t{0}
		);
		aplicNode->associateIrqController(aplic);
	} else {
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
		infoLogger() << "    Hard index " << bspIdx
		             << " corresponds to BSP S-mode external interrupt" << frg::endlog;
		if (ourExternalIrq->type != ExternalIrqType::none)
			panicLogger() << "Multiple APLIC nodes are routed to BSP S-mode external interrupt"
			              << frg::endlog;

		auto aplic = frg::construct<Aplic>(
		    *kernelAlloc, reg.front().addr, reg.front().size, numSources, nullptr, bspIdx
		);
		aplicNode->associateIrqController(aplic);

		ourExternalIrq->type = ExternalIrqType::aplic;
		ourExternalIrq->controller = aplic;
		ourExternalIrq->context = bspIdx;
	}
}

initgraph::Task initPlic{
    &globalInitEngine,
    "riscv.init-aplic",
    initgraph::Requires{getDeviceTreeParsedStage()},
    initgraph::Entails{getTaskingAvailableStage()},
    [] {
	    phandleToImsic.initialize(frg::hash<uint32_t>{}, *kernelAlloc);

	    getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
		    if (node->isCompatible(imsciCompatible))
			    enumerateImsic(node);
		    return false;
	    });
	    getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
		    if (node->isCompatible(aplicCompatible))
			    enumerateAplic(node);
		    return false;
	    });
    }
};

} // namespace

IrqPin *claimImsicIrq() {
	auto idx = riscv::readWriteCsr<riscv::Csr::stopei>(0) >> 16;
	if (!idx)
		return nullptr;

	auto *ourExternalIrq = &riscvExternalIrq.get();
	assert(ourExternalIrq->type == ExternalIrqType::imsic);
	assert(ourExternalIrq->controller);
	auto *ctx = static_cast<ImsicContext *>(ourExternalIrq->controller);
	if (idx >= ctx->irqs.size()) {
		warningLogger() << "thor: IMSIC IRQ index " << idx << " out of bounds" << frg::endlog;
		return nullptr;
	}
	return ctx->irqs[idx];
}

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
