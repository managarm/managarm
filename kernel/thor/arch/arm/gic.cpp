#include <thor-internal/arch/gic.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/system.hpp>
#include <arch/bits.hpp>
#include <arch/mem_space.hpp>
#include <arch/register.hpp>
#include <thor-internal/debug.hpp>
#include <assert.h>
#include <thor-internal/initgraph.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <thor-internal/fiber.hpp>

namespace thor {

namespace {
	static constexpr uint8_t defaultPrio = 0xA0;
} // namespace anonymous

// ---------------------------------------------------------------------
// GicDistributor
// ---------------------------------------------------------------------

namespace dist_reg {
	static constexpr uintptr_t irqGroupBase = 0x80;
	static constexpr uintptr_t irqSetEnableBase = 0x100;
	static constexpr uintptr_t irqClearEnableBase = 0x180;
	static constexpr uintptr_t irqSetPendingBase = 0x200;
	static constexpr uintptr_t irqClearPendingBase = 0x280;
	static constexpr uintptr_t irqPriorityBase = 0x400;
	static constexpr uintptr_t irqTargetBase = 0x800;
	static constexpr uintptr_t irqConfigBase = 0xC00;
	static constexpr uintptr_t sgiSetPendingBase = 0xF10;
	static constexpr uintptr_t sgiClearPendingBase = 0xF20;

	static constexpr arch::bit_register<uint32_t> control{0x00};
	static constexpr arch::bit_register<uint32_t> type{0x04};
	static constexpr arch::bit_register<uint32_t> sgi{0xF00};
} // namespace dist_reg

namespace dist_control {
	arch::field<uint32_t, bool> enable{0, 1};
} // namespace dist_control

namespace dist_type {
	arch::field<uint32_t, uint8_t> noLines{0, 5};
	arch::field<uint32_t, uint8_t> noCpuIface{5, 4};
	arch::field<uint32_t, bool> securityExtensions{10, 1};
} // namespace dist_type

namespace dist_sgi {
	arch::field<uint32_t, uint8_t> sgiNo{0, 4};
	arch::field<uint32_t, uint8_t> cpuTargetList{16, 8};
	arch::field<uint32_t, uint8_t> targetListFilter{24, 2};
} // namespace dist_sgi

GicDistributor::GicDistributor(uintptr_t addr)
: base_{addr}, space_{}, irqPins_{*kernelAlloc} {
	auto register_ptr = KernelVirtualMemory::global().allocate(0x1000);
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), addr,
			page_access::write, CachingMode::uncached);
	space_ = arch::mem_space{register_ptr};
}

void GicDistributor::init() {
	auto type = space_.load_relaxed(dist_reg::type);
	auto noLines = 32 * ((type & dist_type::noLines) + 1);
	auto noCpuIface = (type & dist_type::noCpuIface) + 1;
	bool securityExtensions = type & dist_type::securityExtensions;

	infoLogger() << "GIC Distributor has " << noLines << " IRQs, "
			<< noCpuIface << " CPU interfaces and "
			<< (securityExtensions ? "supports" : "doesn't support") << " security extensions" << frg::endlog;

	space_.store_relaxed(dist_reg::control, dist_control::enable(false));

	auto iface = getCurrentCpuIfaceNo_();

	irqPins_.resize(noLines, nullptr);
	for (int i = 0; i < noLines; i++) {
		auto pin = frg::construct<Pin>(*kernelAlloc, this, i);
		irqPins_[i] = pin;

		if (i >= 32) {
			pin->mask();
			pin->setPriority_(defaultPrio);
			pin->setAffinity_(iface);
		}
	}

	space_.store_relaxed(dist_reg::control, dist_control::enable(true));
}

void GicDistributor::initOnThisCpu() {
	for (int i = 0; i < 32; i++) {
		auto pin = irqPins_[i];
		pin->mask();
		pin->setPriority_(defaultPrio);
		pin->unmask();
	}
}

void GicDistributor::sendIpi(uint8_t ifaceNo, uint8_t id) {
	space_.store_relaxed(dist_reg::sgi, dist_sgi::sgiNo(id) | dist_sgi::cpuTargetList(1 << ifaceNo) | dist_sgi::targetListFilter(0));
}

void GicDistributor::sendIpiToOthers(uint8_t id) {
	space_.store_relaxed(dist_reg::sgi, dist_sgi::sgiNo(id) | dist_sgi::targetListFilter(1));
}

frg::string<KernelAlloc> GicDistributor::buildPinName(uint32_t irq) {
	return frg::string<KernelAlloc>{*kernelAlloc, "gic@0x"}
			+ frg::to_allocated_string(*kernelAlloc, base_, 16)
			+ frg::string<KernelAlloc>{*kernelAlloc, ":"}
			+ frg::to_allocated_string(*kernelAlloc, irq);
}

extern frg::manual_box<IrqSlot> globalIrqSlots[numIrqSlots];

auto GicDistributor::setupIrq(uint32_t irq, TriggerMode trigger) -> Pin * {
	if (irq >= irqPins_.size())
		return nullptr;

	auto pin = irqPins_[irq];
	pin->configure({trigger, Polarity::high});

	return pin;
}

IrqStrategy GicDistributor::Pin::program(TriggerMode mode, Polarity polarity) {
	bool success = setMode(mode, polarity);
	assert(success);

	if (irq_ >= 32)
		setAffinity_(getCpuData()->gicCpuInterface->interfaceNumber());

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

void GicDistributor::Pin::mask() {
	size_t regOff = (irq_ / 32) * 4;
	size_t bitOff = irq_ & 31;

	arch::scalar_store_relaxed<uint32_t>(parent_->space_, dist_reg::irqClearEnableBase + regOff, (1 << bitOff));
}

void GicDistributor::Pin::unmask() {
	size_t regOff = (irq_ / 32) * 4;
	size_t bitOff = irq_ & 31;

	arch::scalar_store_relaxed<uint32_t>(parent_->space_, dist_reg::irqSetEnableBase + regOff, (1 << bitOff));
}

void GicDistributor::Pin::sendEoi() {
	getCpuData()->gicCpuInterface->eoi(0, irq_);
}

void GicDistributor::Pin::setAffinity_(uint8_t ifaceNo) {
	size_t regOff = (irq_ / 4) * 4;
	size_t bitOff = (irq_ & 3) * 8;

	auto v = arch::scalar_load_relaxed<uint32_t>(parent_->space_,
			dist_reg::irqTargetBase + regOff);

	v &= ~(0xFF << bitOff);
	v |= (1 << ifaceNo) << bitOff;

	arch::scalar_store_relaxed<uint32_t>(parent_->space_,
			dist_reg::irqTargetBase + regOff, v);
}

void GicDistributor::Pin::setPriority_(uint8_t prio) {
	size_t regOff = (irq_ / 4) * 4;
	size_t bitOff = (irq_ & 3) * 8;

	auto v = arch::scalar_load_relaxed<uint32_t>(parent_->space_,
			dist_reg::irqPriorityBase + regOff);

	v &= ~(0xFF << bitOff);
	v |= uint32_t(prio) << bitOff;

	arch::scalar_store_relaxed<uint32_t>(parent_->space_,
			dist_reg::irqPriorityBase + regOff, v);
}

bool GicDistributor::Pin::setMode(TriggerMode trigger, Polarity polarity) {
	uintptr_t i = irq_ / 16;
	uintptr_t j = (irq_ % 16) * 2;

	if (irq_ < 16)
		return false;

	if (polarity == Polarity::low)
		return false;

	auto v = arch::scalar_load_relaxed<uint32_t>(parent_->space_,
			dist_reg::irqConfigBase + i * 4);

	v &= ~(3 << j);
	v |= (trigger == TriggerMode::edge ? 2 : 0) << j;

	arch::scalar_store_relaxed<uint32_t>(parent_->space_,
			dist_reg::irqConfigBase + i * 4, v);

	return true;
}

void GicDistributor::dumpPendingSgis() {
	for (int i = 0; i < 16; i++) {
		int off = (i % 4) * 8;
		int reg = (i / 4);

		auto regv = arch::scalar_load_relaxed<uint32_t>(space_, dist_reg::sgiSetPendingBase + reg * 4);

		auto sgiv = (regv >> off) & 0xFF;

		for (int j = 0; j < 8; j++) {
			if (sgiv & (1 << j)) {
				infoLogger() << "thor: on CPU " << getCpuData()->cpuIndex << ", SGI " << i << " pending from CPU " << j << frg::endlog;
			}
		}
	}
}

uint8_t GicDistributor::getCurrentCpuIfaceNo_() {
	for (size_t i = 0; i < 8; i++) {
		auto v = arch::scalar_load_relaxed<uint32_t>(space_, dist_reg::irqTargetBase + i * 4);

		if (!v)
			continue;

		auto mask = ((v >> 24) & 0xFF)
			| ((v >> 16) & 0xFF)
			| ((v >> 8) & 0xFF)
			| (v & 0xFF);

		assert(__builtin_popcount(mask) == 1);

		return __builtin_ctz(mask);
	}

	infoLogger() << "thor: Unable to determine CPU interface number" << frg::endlog;

	return 0;
}

// ---------------------------------------------------------------------
// CpuInterface
// ---------------------------------------------------------------------

namespace cpu_reg {
	arch::bit_register<uint32_t> control{0x00};
	arch::scalar_register<uint32_t> priorityMask{0x04};
	arch::bit_register<uint32_t> ack{0x0C};
	arch::bit_register<uint32_t> eoi{0x10};
	arch::bit_register<uint32_t> deact{0x1000};
	arch::scalar_register<uint32_t> runningPriority{0x14};

	static constexpr uintptr_t activePriorityBase = 0xD0;
} // namespace cpu_reg

namespace cpu_control {
	arch::field<uint32_t, bool> enable{0, 1};
	arch::field<uint32_t, uint8_t> bypass{5, 4};
	arch::field<uint32_t, bool> eoiModeNs{9, 1};
} // namespace cpu_control

namespace cpu_ack_eoi {
	arch::field<uint32_t, uint32_t> irqId{0, 10};
	arch::field<uint32_t, uint8_t> cpuId{10, 3};
} // namespace cpu_control

GicCpuInterface::GicCpuInterface(GicDistributor *dist, uintptr_t addr, size_t size)
: dist_{dist}, space_{}, useSplitEoiDeact_{}, ifaceNo_{} {
	if (size > 0x1000) {
		useSplitEoiDeact_ = true;
		infoLogger() << "thor: Using split EOI/Deactivate mode" << frg::endlog;
	}

	auto ptr = KernelVirtualMemory::global().allocate(size);

	for (size_t i = 0; i < size; i += kPageSize) {
		KernelPageSpace::global().mapSingle4k(VirtualAddr(ptr) + i, addr + i,
				page_access::write, CachingMode::uncached);
	}
	space_ = arch::mem_space{ptr};
}

void GicCpuInterface::init() {
	dist_->initOnThisCpu();

	space_.store_relaxed(cpu_reg::priorityMask, 0xF0);

	for (int i = 0; i < 4; i++)
		arch::scalar_store_relaxed<uint32_t>(space_, cpu_reg::activePriorityBase + i * 4, 0);

	ifaceNo_ = dist_->getCurrentCpuIfaceNo_();

	auto bypass = space_.load_relaxed(cpu_reg::control) & cpu_control::bypass;

	space_.store_relaxed(cpu_reg::control,
			cpu_control::enable(true)
			| cpu_control::bypass(bypass)
			| cpu_control::eoiModeNs(useSplitEoiDeact_));
}

frg::tuple<uint8_t, uint32_t> GicCpuInterface::get() {
	auto v = space_.load_relaxed(cpu_reg::ack);

	if (useSplitEoiDeact_ && (v & cpu_ack_eoi::irqId) < 1020)
		space_.store_relaxed(cpu_reg::eoi, v);

	return {v & cpu_ack_eoi::cpuId, v & cpu_ack_eoi::irqId};
}

void GicCpuInterface::eoi(uint8_t cpuId, uint32_t irqId) {
	if (useSplitEoiDeact_) {
		space_.store_relaxed(cpu_reg::deact, cpu_ack_eoi::cpuId(cpuId) | cpu_ack_eoi::irqId(irqId));
	} else {
		space_.store_relaxed(cpu_reg::eoi, cpu_ack_eoi::cpuId(cpuId) | cpu_ack_eoi::irqId(irqId));
	}
}

uint8_t GicCpuInterface::getCurrentPriority() {
	return space_.load_relaxed(cpu_reg::runningPriority);
}

// --------------------------------------------------------------------
// Initialization
// --------------------------------------------------------------------

frg::manual_box<GicDistributor> dist;

static uintptr_t cpuInterfaceAddr;
static uintptr_t cpuInterfaceSize;

static initgraph::Task initGic{&globalInitEngine, "arm.init-gic",
	initgraph::Requires{getDeviceTreeParsedStage(), getBootProcessorReadyStage()},
	initgraph::Entails{getIrqControllerReadyStage()},
	// Initialize the GIC.
	[] {
		DeviceTreeNode *gicNode = nullptr;
		getDeviceTreeRoot()->forEach([&](DeviceTreeNode *node) -> bool {
			if (node->isCompatible(dtGicCompatible)) {
				gicNode = node;
				return true;
			}

			return false;
		});

		assert(gicNode && "Failed to find GIC");
		infoLogger() << "thor: found the GIC at node \"" << gicNode->path() << "\"" << frg::endlog;
		assert(gicNode->reg().size() >= 2);

		dist.initialize(gicNode->reg()[0].addr);
		dist->init();

		cpuInterfaceAddr = gicNode->reg()[1].addr;
		cpuInterfaceSize = gicNode->reg()[1].size;

		initGicOnThisCpu();
	}
};

initgraph::Stage *getIrqControllerReadyStage() {
	static initgraph::Stage s{&globalInitEngine, "arm.irq-controller-ready"};
	return &s;
}

void initGicOnThisCpu() {
	auto cpuData = getCpuData();

	cpuData->gicCpuInterface = frg::construct<GicCpuInterface>(*kernelAlloc,
			dist.get(),
			cpuInterfaceAddr, cpuInterfaceSize);
	cpuData->gicCpuInterface->init();
}


} // namespace thor
