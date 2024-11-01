#include <thor-internal/arch/gic_v3.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/dtb/dtb.hpp>

namespace thor {

extern frg::manual_box<IrqSlot> globalIrqSlots[numIrqSlots];
extern IrqSpinlock globalIrqSlotsLock;

static frg::manual_box<GicDistributorV3> dist;
static frg::manual_box<frg::vector<GicRedistributorV3, KernelAlloc>> redists;
static frg::manual_box<GicV3> gicV3;

static constexpr uint8_t defaultPrio = 0xA0;

namespace dist_reg {
static constexpr arch::bit_register<uint32_t> control{0x0};
static constexpr arch::bit_register<uint32_t> type{0x4};

static constexpr uintptr_t irqGroupBase = 0x80;
static constexpr uintptr_t irqConfigBase = 0xC00;
static constexpr uintptr_t irqGroupModBase = 0xD00;
static constexpr uintptr_t irqSetEnableBase = 0x100;
static constexpr uintptr_t irqClearEnableBase = 0x180;
static constexpr uintptr_t irqPriorityBase = 0x400;
static constexpr uintptr_t irqRouterBase = 0x6100;
} // namespace dist_reg

namespace dist_control {
static constexpr arch::field<uint32_t, bool> enableGrp0{0, 1};
static constexpr arch::field<uint32_t, bool> enableGrp1Ns{1, 1};
static constexpr arch::field<uint32_t, bool> enableGrp1S{2, 1};
static constexpr arch::field<uint32_t, bool> areS{4, 1};
static constexpr arch::field<uint32_t, bool> areNs{5, 1};
static constexpr arch::field<uint32_t, bool> rwp{30, 1};
} // namespace dist_control

namespace dist_type {
static constexpr arch::field<uint32_t, uint8_t> irqLines{0, 5};
static constexpr arch::field<uint32_t, bool> securityExtensions{10, 1};
} // namespace dist_type

namespace dist_router {
static constexpr arch::field<uint64_t, uint8_t> aff0{0, 8};
static constexpr arch::field<uint64_t, uint8_t> aff1{8, 8};
static constexpr arch::field<uint64_t, uint8_t> aff2{16, 8};
static constexpr arch::field<uint64_t, uint8_t> aff3{32, 8};
} // namespace dist_router

namespace redist_reg {
static constexpr arch::bit_register<uint64_t> type{0x8};
static constexpr arch::bit_register<uint32_t> waker{0x14};
} // namespace redist_reg

namespace redist_waker {
static constexpr arch::field<uint32_t, bool> processorSleep{1, 1};
static constexpr arch::field<uint32_t, bool> childrenAsleep{2, 1};
} // namespace redist_waker

namespace redist_type {
static constexpr arch::field<uint64_t, bool> last{4, 1};
static constexpr arch::field<uint64_t, uint32_t> affinity{32, 32};
} // namespace redist_type

namespace cpu_sre {
static constexpr arch::field<uint64_t, bool> sre{0, 1};
}

namespace cpu_ctlr {
static constexpr arch::field<uint64_t, bool> separateDeact{1, 1};
}

namespace cpu_sgi1r {
static constexpr arch::field<uint64_t, uint16_t> targetList{0, 16};
static constexpr arch::field<uint64_t, uint8_t> aff1{16, 8};
static constexpr arch::field<uint64_t, uint8_t> intId{24, 4};
static constexpr arch::field<uint64_t, uint8_t> aff2{32, 8};
static constexpr arch::field<uint64_t, bool> irm{40, 1};
static constexpr arch::field<uint64_t, uint8_t> aff3{48, 8};
} // namespace cpu_sgi1r

static GicRedistributorV3 &getRedistForThisCpu() {
	auto cpuData = getCpuData();
	auto affinity = cpuData->affinity;

	for (auto &redist : *redists.get()) {
		if (redist.ownedBy(affinity))
			return redist;
	}
	panicLogger() << "thor: GIC redistributor was not found for cpu " << cpuData->cpuIndex
	              << " (affinity " << affinity << ")" << frg::endlog;
	__builtin_unreachable();
}

GicDistributorV3::GicDistributorV3(uintptr_t addr, uintptr_t size) : base_{addr}, space_{} {

	auto ptr = KernelVirtualMemory::global().allocate(size);
	for (size_t i = 0; i < size; i += kPageSize) {
		KernelPageSpace::global().mapSingle4k(
		    VirtualAddr(ptr) + i, addr + i, page_access::write, CachingMode::mmio
		);
	}
	space_ = arch::mem_space{ptr};
}

void GicDistributorV3::init() {
	space_.store_relaxed(dist_reg::control, dist_control::areS(true) | dist_control::areNs(true));

	while (space_.load_relaxed(dist_reg::control) & dist_control::rwp)
		;

	auto control = dist_control::enableGrp0(true) | dist_control::enableGrp1Ns(true) |
	               dist_control::enableGrp1S(true) | dist_control::areS(true) |
	               dist_control::areNs(true);
	space_.store_relaxed(dist_reg::control, control);
}

frg::string<KernelAlloc> GicDistributorV3::buildPinName(uint32_t irq) {
	return frg::string<KernelAlloc>{*kernelAlloc, "gic@0x"} +
	       frg::to_allocated_string(*kernelAlloc, base_, 16) +
	       frg::string<KernelAlloc>{*kernelAlloc, ":"} +
	       frg::to_allocated_string(*kernelAlloc, irq);
}

GicRedistributorV3::GicRedistributorV3(arch::mem_space space) : space_{space} {}

void GicRedistributorV3::initOnThisCpu() {
	auto waker = space_.load_relaxed(redist_reg::waker);
	waker &= ~redist_waker::processorSleep;
	space_.store_relaxed(redist_reg::waker, waker);
	while (space_.load_relaxed(redist_reg::waker) & redist_waker::childrenAsleep)
		;

	arch::scalar_store_relaxed<uint32_t>(space_, 0x10000 + dist_reg::irqGroupBase, ~0);
	arch::scalar_store_relaxed<uint32_t>(space_, 0x10000 + dist_reg::irqGroupModBase, 0);
}

bool GicRedistributorV3::ownedBy(uint32_t affinity) const {
	return (space_.load_relaxed(redist_reg::type) & redist_type::affinity) == affinity;
}

bool GicPinV3::setMode(TriggerMode trigger, Polarity polarity) {
	if (irq_ < 16)
		return false;

	if (polarity == Polarity::low)
		return false;

	auto bitOffset = irq_ % 16 * 2;
	auto offset = irq_ / 16 * 4;

	auto groupOffset = irq_ / 32 * 4;
	auto groupBitOffset = irq_ % 32;

	uint32_t bitValue = trigger == TriggerMode::edge ? 0b10 : 0b00;

	auto space = irq_ < 32 ? getRedistForThisCpu().space_.subspace(0x10000) : dist->space_;

	auto v = arch::scalar_load_relaxed<uint32_t>(space, dist_reg::irqConfigBase + offset);
	v &= ~(0b11 << bitOffset);
	v |= bitValue << bitOffset;
	arch::scalar_store_relaxed(space, dist_reg::irqConfigBase + offset, v);

	auto group = arch::scalar_load_relaxed<uint32_t>(space, dist_reg::irqGroupBase + groupOffset);
	group |= 1U << groupBitOffset;
	arch::scalar_store_relaxed(space, dist_reg::irqGroupBase + groupOffset, group);

	auto groupMod =
	    arch::scalar_load_relaxed<uint32_t>(space, dist_reg::irqGroupModBase + groupOffset);
	groupMod &= ~(1U << groupBitOffset);
	arch::scalar_store_relaxed(space, dist_reg::irqGroupModBase + groupOffset, groupMod);

	return true;
}

IrqStrategy GicPinV3::program(TriggerMode mode, Polarity polarity) {
	auto guard = frg::guard(&globalIrqSlotsLock);

	bool success = setMode(mode, polarity);
	assert(success);

	if (irq_ >= 32)
		setAffinity_(getCpuData()->affinity);

	assert(globalIrqSlots[irq_]->isAvailable());
	globalIrqSlots[irq_]->link(this);

	unmask();

	if (mode == TriggerMode::edge) {
		return IrqStrategy::justEoi;
	} else {
		assert(mode == TriggerMode::level);
		return IrqStrategy::maskThenEoi;
	}
}

void GicPinV3::mask() {
	auto bit = irq_ % 32;
	auto offset = irq_ / 32 * 4;

	auto space = irq_ < 32 ? getRedistForThisCpu().space_.subspace(0x10000) : dist->space_;
	arch::scalar_store_relaxed(space, dist_reg::irqClearEnableBase + offset, 1U << bit);
}

void GicPinV3::unmask() {
	auto bit = irq_ % 32;
	auto offset = irq_ / 32 * 4;

	auto space = irq_ < 32 ? getRedistForThisCpu().space_.subspace(0x10000) : dist->space_;
	arch::scalar_store_relaxed(space, dist_reg::irqSetEnableBase + offset, 1U << bit);
}

void GicPinV3::sendEoi() { gicV3->eoi(0, irq_); }

void GicPinV3::setAffinity_(uint32_t affinity) {
	if (irq_ < 32)
		return;

	auto offset = (irq_ - 32) * 8;

	arch::bit_value<uint64_t> v = dist_router::aff0(affinity) | dist_router::aff1(affinity >> 8) |
	                              dist_router::aff2(affinity >> 16) |
	                              dist_router::aff3(affinity >> 24);

	arch::scalar_store_relaxed(
	    dist->space_, dist_reg::irqRouterBase + offset, static_cast<uint64_t>(v)
	);
}

void GicPinV3::setPriority_(uint8_t priority) {
	auto offset = irq_ / 4 * 4;
	auto bitOffset = irq_ % 4 * 8;

	auto space = irq_ < 32 ? getRedistForThisCpu().space_.subspace(0x10000) : dist->space_;

	auto value = arch::scalar_load_relaxed<uint32_t>(space, dist_reg::irqPriorityBase + offset);
	value &= ~(0xFF << bitOffset);
	value |= static_cast<uint32_t>(priority) << bitOffset;
	arch::scalar_store_relaxed(space, dist_reg::irqPriorityBase + offset, value);
}

bool initGicV3() {
	DeviceTreeNode *gicNode = nullptr;
	getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
		if (node->isCompatible(dtGicV3Compatible)) {
			gicNode = node;
			return true;
		}

		return false;
	});

	if (!gicNode)
		return false;

	infoLogger() << "thor: found the GIC at node \"" << gicNode->path() << "\"" << frg::endlog;
	assert(gicNode->reg().size() >= 2);

	auto reg = gicNode->reg();
	dist.initialize(reg[0].addr, reg[0].size);

	auto redistPtr = KernelVirtualMemory::global().allocate(reg[1].size);
	for (size_t i = 0; i < reg[1].size; i += kPageSize) {
		KernelPageSpace::global().mapSingle4k(
		    VirtualAddr(redistPtr) + i, reg[1].addr + i, page_access::write, CachingMode::mmio
		);
	}

	auto redistCount = reg[1].size / 0x20000;
	redists.initialize(*kernelAlloc);

	for (size_t i = 0; i < redistCount; ++i) {
		arch::mem_space space{(void *)(VirtualAddr(redistPtr) + i * 0x20000)};
		redists->emplace_back(space);

		if (space.load_relaxed(redist_reg::type) & redist_type::last)
			break;
	}

	dist->init();

	gicV3.initialize();
	gic = gicV3.get();

	initGicOnThisCpuV3();

	return true;
}

void initGicOnThisCpuV3() {
	getRedistForThisCpu().initOnThisCpu();

	arch::bit_value<uint64_t> sre{0};
	asm volatile("mrs %0, icc_sre_el1" : "=r"(sre));
	sre |= cpu_sre::sre(true);
	asm volatile("msr icc_sre_el1, %0" : : "r"(sre));

	arch::bit_value<uint64_t> ctlr{0};
	asm volatile("mrs %0, icc_ctlr_el1" : "=r"(ctlr));
	ctlr |= cpu_ctlr::separateDeact(true);
	asm volatile("msr icc_ctlr_el1, %0" : : "r"(ctlr));

	uint64_t priority = 0xFF;
	asm volatile("msr icc_pmr_el1, %0" : : "r"(priority));

	// No pre-emption
	uint64_t bpr = 0b111;
	asm volatile("msr icc_bpr1_el1, %0" : : "r"(bpr));

	uint64_t igrpen1;
	asm volatile("mrs %0, icc_igrpen1_el1" : "=r"(igrpen1));
	igrpen1 |= 1;
	asm volatile("msr icc_igrpen1_el1, %0" : : "r"(igrpen1));

	for (int i = 0; i < 32; ++i) {
		auto pin = static_cast<GicPinV3 *>(gicV3->getPin(i));
		pin->mask();
		pin->setPriority_(defaultPrio);
		if (i < 16)
			pin->unmask();
	}
}

GicV3::GicV3() : irqPins_{*kernelAlloc} {
	auto affinity = getCpuData()->affinity;

	auto type = dist->space_.load_relaxed(dist_reg::type);
	uint32_t irqLines = type & dist_type::irqLines;
	auto securityExtensions = type & dist_type::securityExtensions;

	// TODO: there can be more extension pins
	auto pins = frg::min<uint32_t>(32 * (irqLines + 1), 1020);

	infoLogger() << "GIC Distributor has " << pins << " IRQs and "
	             << (securityExtensions ? "supports" : "doesn't support") << " security extensions"
	             << frg::endlog;

	irqPins_.resize(pins);
	for (uint32_t i = 0; i < pins; ++i) {
		irqPins_[i] = frg::construct<GicPinV3>(*kernelAlloc, dist.get(), i);

		if (i >= 32) {
			irqPins_[i]->mask();
			irqPins_[i]->setPriority_(defaultPrio);
			irqPins_[i]->setAffinity_(affinity);
		}
	}
}

void GicV3::sendIpi(int cpuId, uint8_t id) {
	auto affinity = getCpuData(cpuId)->affinity;
	uint8_t aff0 = affinity;
	uint8_t aff1 = affinity >> 8;
	uint8_t aff2 = affinity >> 16;
	uint8_t aff3 = affinity >> 24;

	arch::bit_value<uint64_t> v = cpu_sgi1r::targetList(1U << aff0) | cpu_sgi1r::aff1(aff1) |
	                              cpu_sgi1r::aff2(aff2) | cpu_sgi1r::aff3(aff3) |
	                              cpu_sgi1r::intId(id);
	asm volatile("msr icc_sgi1r_el1, %0" : : "r"(v));
}

void GicV3::sendIpiToOthers(uint8_t id) {
	arch::bit_value<uint64_t> v = cpu_sgi1r::irm(true) | cpu_sgi1r::intId(id);
	asm volatile("msr icc_sgi1r_el1, %0" : : "r"(v));
}

Gic::CpuIrq GicV3::getIrq() {
	uint64_t iar1;
	asm volatile("mrs %0, icc_iar1_el1" : "=r"(iar1));
	uint32_t irq = iar1 & 0xFFFFFF;

	if (irq < 1020)
		asm volatile("msr icc_eoir1_el1, %0" : : "r"(uint64_t{irq}));

	return {0, irq};
}

void GicV3::eoi(uint32_t, uint32_t id) {
	asm volatile("msr icc_dir_el1, %0" : : "r"(uint64_t{id}));
}

Gic::Pin *GicV3::setupIrq(uint32_t irq, TriggerMode trigger) {
	if (irq >= irqPins_.size())
		return nullptr;

	auto pin = irqPins_[irq];
	pin->configure({trigger, Polarity::high});

	return pin;
}

Gic::Pin *GicV3::getPin(uint32_t irq) {
	if (irq >= irqPins_.size())
		return nullptr;

	return irqPins_[irq];
}

} // namespace thor
