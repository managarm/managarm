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

namespace thor {

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
	arch::field<uint32_t, bool> enableGroup0{0, 1};
	arch::field<uint32_t, bool> enableGroup1{1, 1};
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
: base_{addr}, space_{}, irq_pins_{*kernelAlloc} {
	auto register_ptr = KernelVirtualMemory::global().allocate(0x1000);
	// TODO: this should use a proper caching mode
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), addr,
			page_access::write, CachingMode::null);
	space_ = arch::mem_space{register_ptr};
}

void GicDistributor::init() {
	auto type = space_.load(dist_reg::type);
	auto noLines = 32 * ((type & dist_type::noLines) + 1);
	auto noCpuIface = (type & dist_type::noCpuIface) + 1;
	auto securityExtensions = type & dist_type::securityExtensions;

	infoLogger() << "GIC Distributor has " << noLines << " IRQs, "
			<< noCpuIface << " CPU interfaces and "
			<< (securityExtensions ? "supports" : "doesn't support") << " security extensions" << frg::endlog;

	assert(!securityExtensions && "Security extensions are not supported");

	space_.store(dist_reg::control, dist_control::enableGroup0(true) | dist_control::enableGroup1(true));

	// Enable all interrupts
	for (size_t i = 0; i < noLines / 32; i++) {
		arch::scalar_store<uint32_t>(space_, dist_reg::irqSetEnableBase + i * 4, 0xFFFFFFFF);
	}

	// All interrupts go to CPU interface 0
	// SGIs and PPIs are read-only and go to the proper CPU interface
	for (size_t i = 8; i < noLines / 4; i++) {
		arch::scalar_store<uint32_t>(space_, dist_reg::irqTargetBase + i * 4, 0x01010101);
	}

	// All interrupts have the same priority
	for (size_t i = 0; i < noLines / 4; i++) {
		arch::scalar_store<uint32_t>(space_, dist_reg::irqPriorityBase + i * 4, 0x00000000);
	}

	// All interrupts are group 0
	for (size_t i = 0; i < noLines / 32; i++) {
		arch::scalar_store<uint32_t>(space_, dist_reg::irqGroupBase + i * 4, 0x00000000);
	}
}

void GicDistributor::initOnThisCpu() {
	// Set banked interrupt enable
	arch::scalar_store<uint32_t>(space_, dist_reg::irqSetEnableBase, 0xFFFFFFFF);

	// Set banked interrupt priority
	for (size_t i = 0; i < 8; i++) {
		arch::scalar_store<uint32_t>(space_, dist_reg::irqPriorityBase + i * 4, 0x00000000);
	}

	// All banked interrupts are group 0
	arch::scalar_store<uint32_t>(space_, dist_reg::irqGroupBase, 0x00000000);
}

void GicDistributor::sendIpi(uint8_t cpu, uint8_t id) {
	space_.store(dist_reg::sgi, dist_sgi::sgiNo(id) | dist_sgi::cpuTargetList(1 << cpu) | dist_sgi::targetListFilter(0));
}

frg::string<KernelAlloc> GicDistributor::buildPinName(uint32_t irq) {
	return frg::string<KernelAlloc>{*kernelAlloc, "gic@0x"}
			+ frg::to_allocated_string(*kernelAlloc, base_, 16)
			+ frg::string<KernelAlloc>{*kernelAlloc, ":"}
			+ frg::to_allocated_string(*kernelAlloc, irq);
}

extern frg::manual_box<IrqSlot> globalIrqSlots[numIrqSlots];

auto GicDistributor::setupIrq(uint32_t irq, TriggerMode trigger) -> Pin * {
	auto pin = frg::construct<Pin>(*kernelAlloc, this, irq);
	pin->configure({trigger, Polarity::high});
	irq_pins_.push_back(pin);

	return pin;
}

IrqStrategy GicDistributor::Pin::program(TriggerMode mode, Polarity) {
	parent_->configureTrigger(irq_, mode);
	assert(globalIrqSlots[irq_]->isAvailable());
	globalIrqSlots[irq_]->link(this);

	if (mode == TriggerMode::edge) {
		return IrqStrategy::justEoi;
	} else {
		assert(mode == TriggerMode::level);
		return IrqStrategy::maskThenEoi;
	}
}

void GicDistributor::Pin::mask() { /* TODO */ }
void GicDistributor::Pin::unmask() { /* TODO */ }

// TODO: this should be per-cpu
frg::manual_box<GicCpuInterface> cpuInterface;

void GicDistributor::Pin::sendEoi() {
	cpuInterface->eoi(0, irq_);
}

void GicDistributor::configureTrigger(uint32_t irq, TriggerMode trigger) {
	uintptr_t i = irq / 16;
	uintptr_t j = irq % 16;

	auto v = arch::scalar_load<uint32_t>(space_, dist_reg::irqConfigBase + i * 4);
	v |= (trigger == TriggerMode::edge) << ((j * 2) + 1);
	arch::scalar_store<uint32_t>(space_, dist_reg::irqConfigBase + i * 4, v);
}

// ---------------------------------------------------------------------
// CpuInterface
// ---------------------------------------------------------------------

namespace cpu_reg {
	arch::bit_register<uint32_t> control{0x00};
	arch::scalar_register<uint32_t> priorityMask{0x04};
	arch::scalar_register<uint32_t> binaryPoint{0x08};
	arch::bit_register<uint32_t> ack{0x0C};
	arch::bit_register<uint32_t> eoi{0x10};
} // namespace cpu_reg

namespace cpu_control {
	arch::field<uint32_t, bool> enableGroup0{0, 1};
	arch::field<uint32_t, bool> enableGroup1{1, 1};
	arch::field<uint32_t, bool> ackControl{2, 1};
	arch::field<uint32_t, bool> fiqEnable{3, 1};
	arch::field<uint32_t, bool> commonBinaryPoint{4, 1};
	arch::field<uint32_t, bool> eoiMode{9, 1};
} // namespace cpu_control

namespace cpu_ack_eoi {
	arch::field<uint32_t, uint32_t> irqId{0, 10};
	arch::field<uint32_t, uint8_t> cpuId{10, 3};
} // namespace cpu_control

GicCpuInterface::GicCpuInterface(GicDistributor *dist, uintptr_t addr)
: dist_{dist}, space_{} {
	auto register_ptr = KernelVirtualMemory::global().allocate(0x1000);
	// TODO: this should use a proper caching mode
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), addr,
			page_access::write, CachingMode::null);
	space_ = arch::mem_space{register_ptr};
}

void GicCpuInterface::init() {
	dist_->initOnThisCpu();

	space_.store(cpu_reg::control,
			cpu_control::enableGroup0(true)
			| cpu_control::enableGroup1(true)
			| cpu_control::ackControl(true)
			| cpu_control::fiqEnable(false)
			| cpu_control::commonBinaryPoint(true)
			| cpu_control::eoiMode(false));

	space_.store(cpu_reg::priorityMask, 0xFF);
	space_.store(cpu_reg::binaryPoint, 7);
}

frg::tuple<uint8_t, uint32_t> GicCpuInterface::get() {
	auto v = space_.load(cpu_reg::ack);
	return {v & cpu_ack_eoi::cpuId, v & cpu_ack_eoi::irqId};
}

void GicCpuInterface::eoi(uint8_t cpuId, uint32_t irqId) {
	space_.store(cpu_reg::eoi, cpu_ack_eoi::cpuId(cpuId) | cpu_ack_eoi::irqId(irqId));
}

// --------------------------------------------------------------------
// Initialization
// --------------------------------------------------------------------

frg::manual_box<GicDistributor> dist;

static initgraph::Task initGic{&basicInitEngine, "arm.init-gic",
	initgraph::Entails{getIrqControllerReadyStage()},
	// Initialize the GIC.
	[] {
		// TODO: get these addresses from dtb

		dist.initialize(0x08000000);
		dist->init();

		// TODO: do this for each cpu
		cpuInterface.initialize(dist.get(), 0x08010000);
		cpuInterface->init();
	}
};

initgraph::Stage *getIrqControllerReadyStage() {
	static initgraph::Stage s{&basicInitEngine, "arm.irq-controller-ready"};
	return &s;
}

} // namespace thor
