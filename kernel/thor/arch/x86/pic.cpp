#include <arch/bits.hpp>
#include <arch/io_space.hpp>
#include <arch/mem_space.hpp>
#include <arch/register.hpp>
#include <thor-internal/arch/hpet.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/fiber.hpp>
#include <initgraph.hpp>
#include <thor-internal/irq.hpp>
#include <thor-internal/main.hpp>

namespace thor {

namespace {
	constexpr bool debugTimer = false;
}

inline constexpr arch::bit_register<uint32_t> lApicId(0x0020);
inline constexpr arch::scalar_register<uint32_t> lApicEoi(0x00B0);
inline constexpr arch::bit_register<uint32_t> lApicSpurious(0x00F0);
inline constexpr arch::bit_register<uint32_t> lApicIcrLow(0x0300);
inline constexpr arch::bit_register<uint32_t> lApicIcrHigh(0x0310);
inline constexpr arch::bit_register<uint64_t> lX2ApicIcr{0x0300};
inline constexpr arch::bit_register<uint32_t> lApicLvtTimer(0x0320);
inline constexpr arch::bit_register<uint32_t> lApicLvtPerfCount(0x0340);
inline constexpr arch::bit_register<uint32_t> lApicLvtLocal0(0x0350);
inline constexpr arch::bit_register<uint32_t> lApicLvtLocal1(0x0360);
inline constexpr arch::scalar_register<uint32_t> lApicInitCount(0x0380);
inline constexpr arch::scalar_register<uint32_t> lApicCurCount(0x0390);

// lApicId registers
inline constexpr arch::field<uint32_t, uint8_t> apicId(24, 8);
inline constexpr arch::field<uint32_t, uint8_t> x2ApicId(0, 8);

// lApicSpurious registers
inline constexpr arch::field<uint32_t, uint8_t> apicSpuriousVector(0, 8);
inline constexpr arch::field<uint32_t, bool> apicSpuriousSwEnable(8, 1);
inline constexpr arch::field<uint32_t, bool> apicSpuriousFocusProcessor(9, 1);
inline constexpr arch::field<uint32_t, bool> apicSpuriousEoiBroadcastSuppression(12, 1);

// lApicIcrLow registers
inline constexpr arch::field<uint32_t, uint8_t> apicIcrLowVector(0, 8);
inline constexpr arch::field<uint32_t, uint8_t> apicIcrLowDelivMode(8, 3);
inline constexpr arch::field<uint32_t, bool> apicIcrLowDestMode(11, 1);
inline constexpr arch::field<uint32_t, bool> apicIcrLowDelivStatus(12, 1);
inline constexpr arch::field<uint32_t, bool> apicIcrLowLevel(14, 1);
inline constexpr arch::field<uint32_t, bool> apicIcrLowTriggerMode(15, 1);
inline constexpr arch::field<uint32_t, uint8_t> apicIcrLowShorthand(18, 2);

// lApicIcrHigh registers
inline constexpr arch::field<uint32_t, uint8_t> apicIcrHighDestField(24, 8);

// lX2ApicIcr registers
inline constexpr arch::field<uint64_t, uint8_t> x2apicIcrLowVector(0, 8);
inline constexpr arch::field<uint64_t, uint8_t> x2apicIcrLowDelivMode(8, 3);
inline constexpr arch::field<uint64_t, bool> x2apicIcrLowDestMode(11, 1);
inline constexpr arch::field<uint64_t, bool> x2apicIcrLowDelivStatus(12, 1);
inline constexpr arch::field<uint64_t, bool> x2apicIcrLowLevel(14, 1);
inline constexpr arch::field<uint64_t, bool> x2apicIcrLowTriggerMode(15, 1);
inline constexpr arch::field<uint64_t, uint8_t> x2apicIcrLowShorthand(18, 2);
inline constexpr arch::field<uint64_t, uint32_t> x2apicIcrHighDestField(32, 32);

// lApicLvtTimer registers
inline constexpr arch::field<uint32_t, uint8_t> apicLvtVector(0, 8);
inline constexpr arch::field<uint32_t, bool> apicLvtMask(16, 1);
inline constexpr arch::field<uint32_t, uint8_t> apicLvtMode(8, 3);
inline constexpr arch::field<uint32_t, uint8_t> apicLvtTimerMode(17, 2);

ApicRegisterSpace picBase;

namespace {
	bool getLocalApicIsr(unsigned int vector) {
		arch::scalar_register<uint32_t> isrRegister{0x100 + 0x10 * (vector >> 5)};
		return picBase.load(isrRegister) & (1 << (vector & 31));
	}

	bool getLocalApicTmr(unsigned int vector) {
		arch::scalar_register<uint32_t> tmrRegister{0x180 + 0x10 * (vector >> 5)};
		return picBase.load(tmrRegister) & (1 << (vector & 31));
	}

	bool getLocalApicIrr(unsigned int vector) {
		arch::scalar_register<uint32_t> irrRegister{0x200 + 0x10 * (vector >> 5)};
		return picBase.load(irrRegister) & (1 << (vector & 31));
	}
}

enum {
	kModelLegacy = 1,
	kModelApic = 2
};

static int picModel = kModelLegacy;

uint64_t getRawTimestampCounter() {
	uint32_t lsw, msw;
	asm volatile ("rdtsc" : "=a"(lsw), "=d"(msw));
	return (static_cast<uint64_t>(msw) << 32)
			| static_cast<uint64_t>(lsw);
}

// --------------------------------------------------------
// Local APIC timer
// --------------------------------------------------------

namespace {
	frg::eternal<GlobalApicContext> globalApicContextInstance;

	LocalApicContext *localApicContext() {
		return &getCpuData()->apicContext;
	}
}

GlobalApicContext *globalApicContext() {
	return &globalApicContextInstance.get();
}

void GlobalApicContext::GlobalAlarmSlot::arm(uint64_t nanos) {
	assert(localApicContext()->timersAreCalibrated);

	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&globalApicContext()->_mutex);
		globalApicContext()->_globalDeadline = nanos;
	}
	LocalApicContext::_updateLocalTimer();
}

LocalApicContext::LocalApicContext()
: _preemptionDeadline{0}, _globalDeadline{0} { }

void LocalApicContext::setPreemption(uint64_t nanos) {
	assert(localApicContext()->timersAreCalibrated);

	localApicContext()->_preemptionDeadline = nanos;
	LocalApicContext::_updateLocalTimer();
}

bool LocalApicContext::checkPreemption() {
	return localApicContext()->_preemptionDeadline != 0;
}

void LocalApicContext::handleTimerIrq() {
	assert(localApicContext()->timersAreCalibrated);

	if(debugTimer)
		infoLogger() << "thor [CPU " << getLocalApicId() << "]: Timer IRQ triggered"
				<< frg::endlog;
	auto self = localApicContext();
	auto now = systemClockSource()->currentNanos();

	if(self->_preemptionDeadline && now > self->_preemptionDeadline)
		self->_preemptionDeadline = 0;

	if(self->_globalDeadline && now > self->_globalDeadline) {
		self->_globalDeadline = 0;
		globalApicContext()->_globalAlarmInstance.fireAlarm();

		// Update the global deadline to avoid calling fireAlarm() on the next IRQ.
		{
			auto irq_lock = frg::guard(&irqMutex());
			auto lock = frg::guard(&globalApicContext()->_mutex);
			localApicContext()->_globalDeadline = globalApicContext()->_globalDeadline;
		}
	}

	localApicContext()->_updateLocalTimer();
}

void LocalApicContext::_updateLocalTimer() {
	uint64_t deadline = 0;
	auto consider = [&] (uint64_t dc) {
		if(!dc)
			return;
		if(!deadline || dc < deadline)
			deadline = dc;
	};

	// Copy the global deadline so we can access it without locking.
	{
		auto irq_lock = frg::guard(&irqMutex());
		auto lock = frg::guard(&globalApicContext()->_mutex);
		localApicContext()->_globalDeadline = globalApicContext()->_globalDeadline;
	}

	consider(localApicContext()->_preemptionDeadline);
	consider(localApicContext()->_globalDeadline);

	if(localApicContext()->useTscMode) {
		if(!deadline) {
			common::x86::wrmsr(common::x86::kMsrIa32TscDeadline, 0);
			return;
		}

		uint64_t ticks;
		auto of = __builtin_mul_overflow(deadline, localApicContext()->tscTicksPerMilli, &ticks);
		assert(!of);
		ticks /= 1'000'000;
		common::x86::wrmsr(common::x86::kMsrIa32TscDeadline, ticks);
		if(debugTimer)
			infoLogger() << "thor [CPU " << getLocalApicId() << "]: Setting TSC deadline to "
					<< ticks << frg::endlog;
	}else{
		if(!deadline) {
			picBase.store(lApicInitCount, 0);
			return;
		}

		auto now = systemClockSource()->currentNanos();
		uint64_t ticks;
		if(deadline < now) {
			if(debugTimer)
				infoLogger() << "thor [CPU " << getLocalApicId()
						<< "]: Setting single tick timer" << frg::endlog;
			ticks = 1;
		}else{
			if(debugTimer)
				infoLogger() << "thor [CPU " << getLocalApicId() << "]: Setting timer "
						<< ((deadline - now)/1000) << " us in the future" << frg::endlog;
			auto of = __builtin_mul_overflow(deadline - now,
					localApicContext()->localTicksPerMilli, &ticks);
			assert(!of);
			ticks /= 1'000'000;
			if(!ticks)
				ticks = 1;
		}
		picBase.store(lApicInitCount, ticks);
	}
}

void armPreemption(uint64_t nanos) {
	LocalApicContext::setPreemption(systemClockSource()->currentNanos() + nanos);
}

void disarmPreemption() {
	LocalApicContext::setPreemption(0);
}

bool preemptionIsArmed() {
	return LocalApicContext::checkPreemption();
}

// --------------------------------------------------------
// Local PIC management
// --------------------------------------------------------

initgraph::Stage *getApicDiscoveryStage() {
	static initgraph::Stage s{&globalInitEngine, "x86.apic-discovered"};
	return &s;
}

static initgraph::Task discoverApicTask{&globalInitEngine, "x86.discover-apic",
	initgraph::Entails{getApicDiscoveryStage()},
	[] {
		uint64_t msr = common::x86::rdmsr(common::x86::kMsrLocalApicBase);
		msr |= (1 << 11); // Enable APIC

		bool haveX2apic = false;
		if(common::x86::cpuid(0x01)[2] & (uint32_t(1) << 21)){
			infoLogger() << "\e[37mthor: CPU supports x2apic\e[39m" << frg::endlog;
			msr |= (1 << 10);
			haveX2apic = true;
		} else {
			infoLogger() << "\e[37mthor: CPU does not support x2apic\e[39m" << frg::endlog;
		}

		common::x86::wrmsr(common::x86::kMsrLocalApicBase, msr);

		// TODO: We really only need a single page.
		auto register_ptr = KernelVirtualMemory::global().allocate(0x10000);
		// TODO: Intel SDM specifies that we should mask out all
		// bits > the physical address limit of the msr.
		// For now we just assume that they are zero.
		KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), msr & ~PhysicalAddr{0xFFF},
				page_access::write, CachingMode::null);
		picBase = ApicRegisterSpace(haveX2apic, register_ptr);
	}
};

void initLocalApicPerCpu() {
	uint64_t msr = common::x86::rdmsr(common::x86::kMsrLocalApicBase);
	msr |= (1 << 11); // Enable APIC

	if(picBase.isUsingX2apic()){
		assert(common::x86::cpuid(0x01)[2] & (uint32_t(1) << 21));
		msr |= (1 << 10);
	}

	common::x86::wrmsr(common::x86::kMsrLocalApicBase, msr);

	auto dumpLocalInt = [&] (int index) {
		auto regstr = (index == 0 ? lApicLvtLocal0 : lApicLvtLocal1);
		auto lvt = picBase.load(regstr);
		infoLogger() << "thor: CPU #" << getLocalApicId()
				<< " LINT " << index << " mode is " << (lvt & apicLvtMode)
				<< ", it is " << ((lvt & apicLvtMask) ? "masked" : "not masked")
				<< frg::endlog;
	};

	// Enable the local APIC.
	uint32_t spurious_vector = 0x81;
	picBase.store(lApicSpurious, apicSpuriousVector(spurious_vector)
			| apicSpuriousSwEnable(true));

	dumpLocalInt(0);
	dumpLocalInt(1);

	if(getGlobalCpuFeatures()->haveInvariantTsc
			&& getGlobalCpuFeatures()->haveTscDeadline)
		localApicContext()->useTscMode = true;

	// Setup a timer interrupt for scheduling.
	if(localApicContext()->useTscMode) {
		picBase.store(lApicLvtTimer, apicLvtVector(0xFF) | apicLvtTimerMode(2));
		// The SDM requires this to order MMIO and MSR writes.
		asm volatile ("mfence" : : : "memory");
	}else{
		picBase.store(lApicLvtTimer, apicLvtVector(0xFF));
	}

	// Setup the PMI.
	picBase.store(lApicLvtPerfCount, apicLvtMode(4));

	calibrateApicTimer();
}

uint32_t getLocalApicId() {
	if (picBase.isUsingX2apic()) {
		return picBase.load(lApicId) & x2ApicId;
	} else {
		return picBase.load(lApicId) & apicId;
	}
}

uint64_t localTicks() {
	assert(!localApicContext()->useTscMode);
	return picBase.load(lApicCurCount);
}

namespace {
	struct TscClockSource final : ClockSource {
		uint64_t currentNanos() override {
			auto r = getRawTimestampCounter() * 1'000'000 / localApicContext()->tscTicksPerMilli;
	//		infoLogger() << r << frg::endlog;
			return r;
		}
	};

	frg::manual_box<TscClockSource> globalTscClockSource;
}

extern ClockSource *hpetClockSource;
extern AlarmTracker *hpetAlarmTracker;
extern ClockSource *globalClockSource;
extern PrecisionTimerEngine *globalTimerEngine;

void calibrateApicTimer() {
	const uint64_t millis = 100;

	// Calibrate the local APIC timer.
	if(!localApicContext()->useTscMode) {
		picBase.store(lApicInitCount, 0xFFFFFFFF);
		pollSleepNano(millis * 1'000'000);
		uint32_t elapsed = 0xFFFFFFFF
				- picBase.load(lApicCurCount);
		picBase.store(lApicInitCount, 0);

		localApicContext()->localTicksPerMilli = elapsed / millis;
		infoLogger() << "thor: Local APIC ticks/ms: "
				<< localApicContext()->localTicksPerMilli
				<< " on CPU #" << getCpuData()->cpuIndex << frg::endlog;
	}

	// Calibrate the TSC.
	auto tsc_start = getRawTimestampCounter();
	pollSleepNano(millis * 1'000'000);
	auto tsc_elapsed = getRawTimestampCounter() - tsc_start;

	localApicContext()->tscTicksPerMilli = tsc_elapsed / millis;
	infoLogger() << "thor: TSC ticks/ms: " << localApicContext()->tscTicksPerMilli
				<< " on CPU #" << getCpuData()->cpuIndex << frg::endlog;

	localApicContext()->timersAreCalibrated = true;
}

static initgraph::Task assessTimersTask{&globalInitEngine, "x86.assess-timers",
	initgraph::Requires{getHpetInitializedStage()},
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		if(getGlobalCpuFeatures()->haveInvariantTsc) {
			globalTscClockSource.initialize();
			globalClockSource = globalTscClockSource.get();
		}else{
			infoLogger() << "thor: No invariant TSC; using HPET as system clock source"
					<< frg::endlog;

			globalClockSource = hpetClockSource;
		}

		globalTimerEngine = frg::construct<PrecisionTimerEngine>(*kernelAlloc,
				globalClockSource, globalApicContext()->globalAlarm());
	//			globalClockSource, hpetAlarmTracker);
	}
};

void acknowledgeIpi() {
	picBase.store(lApicEoi, 0);
}

void raiseInitAssertIpi(uint32_t dest_apic_id) {
	if(picBase.isUsingX2apic()){
		picBase.store(lX2ApicIcr, x2apicIcrLowDelivMode(5)
			| x2apicIcrLowLevel(true) | x2apicIcrLowTriggerMode(true) | x2apicIcrHighDestField(dest_apic_id));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
		// DM:init = 5, Level:assert = 1, TM:Level = 1
		picBase.store(lApicIcrLow, apicIcrLowDelivMode(5)
				| apicIcrLowLevel(true) | apicIcrLowTriggerMode(true));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

void raiseInitDeassertIpi(uint32_t dest_apic_id) {
	if(picBase.isUsingX2apic()) {
		picBase.store(lX2ApicIcr, x2apicIcrLowDelivMode(5)
			| x2apicIcrLowTriggerMode(true) | x2apicIcrHighDestField(dest_apic_id));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
		// DM:init = 5, TM:Level = 1
		picBase.store(lApicIcrLow, apicIcrLowDelivMode(5)
				| apicIcrLowTriggerMode(true));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page) {
	assert((page % 0x1000) == 0);
	uint32_t vector = page / 0x1000; // determines the startup code page
	if(picBase.isUsingX2apic()) {
		picBase.store(lX2ApicIcr, x2apicIcrLowVector(vector)
				| x2apicIcrLowDelivMode(6) | x2apicIcrHighDestField(dest_apic_id));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
		// DM:startup = 6
		picBase.store(lApicIcrLow, apicIcrLowVector(vector)
				| apicIcrLowDelivMode(6));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

void sendShootdownIpi() {
	if(picBase.isUsingX2apic()) {
		picBase.store(lX2ApicIcr, x2apicIcrLowVector(0xF0) | x2apicIcrLowDelivMode(0)
				| x2apicIcrLowLevel(true) | x2apicIcrLowShorthand(2) | x2apicIcrHighDestField(0));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(0));
		picBase.store(lApicIcrLow, apicIcrLowVector(0xF0) | apicIcrLowDelivMode(0)
				| apicIcrLowLevel(true) | apicIcrLowShorthand(2));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}	
	}
}

void sendPingIpi(int id) {
	auto apic = getCpuData(id)->localApicId;
//	infoLogger() << "thor [CPU" << getLocalApicId() << "]: Sending ping" << frg::endlog;
	if(picBase.isUsingX2apic()) {
		picBase.store(lX2ApicIcr, x2apicIcrLowVector(0xF1) | x2apicIcrLowDelivMode(0)
				| x2apicIcrLowLevel(true) | x2apicIcrLowShorthand(0) | x2apicIcrHighDestField(apic));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(apic));
		picBase.store(lApicIcrLow, apicIcrLowVector(0xF1) | apicIcrLowDelivMode(0)
				| apicIcrLowLevel(true) | apicIcrLowShorthand(0));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

void sendGlobalNmi() {
	// Send the NMI to all /other/ CPUs but not to the current one.
	if(picBase.isUsingX2apic()) {
		picBase.store(lX2ApicIcr, x2apicIcrLowVector(0) | x2apicIcrLowDelivMode(4)
				| x2apicIcrLowLevel(true) | x2apicIcrLowShorthand(3) | x2apicIcrHighDestField(0));
	} else {
		picBase.store(lApicIcrHigh, apicIcrHighDestField(0));
		picBase.store(lApicIcrLow, apicIcrLowVector(0) | apicIcrLowDelivMode(4)
				| apicIcrLowLevel(true) | apicIcrLowShorthand(3));
		while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
			// Wait for IPI delivery.
		}
	}
}

// --------------------------------------------------------

IrqPin *globalSystemIrqs[256];

IrqPin *getGlobalSystemIrq(size_t n) {
	assert(n <= 256);
	return globalSystemIrqs[n];
}

// --------------------------------------------------------
// MSI management
// --------------------------------------------------------

// TODO: Replace this by proper IRQ allocation.
extern frg::manual_box<IrqSlot> globalIrqSlots[64];
extern IrqSpinlock globalIrqSlotsLock;

namespace {
	struct ApicMsiPin final : MsiPin {
		ApicMsiPin(frg::string<KernelAlloc> name, unsigned int vector)
		: MsiPin{std::move(name)}, vector_{vector} { }

		IrqStrategy program(TriggerMode mode, Polarity) override {
			assert(mode == TriggerMode::edge);
			return IrqStrategy::justEoi;
		}

		void mask() override {
			// TODO: Support this.
			infoLogger() << "\e[31m" "thor: Masking of APIC-MSIs is not implemented" "\e[39m"
					<< frg::endlog;
		}

		void unmask() override {
			// TODO: Implement this when mask() is implemented.
		}

		void sendEoi() override {
			acknowledgeIrq(0);
		}

		uint64_t getMessageAddress() override {
			return 0xFEE00000;
		}

		uint32_t getMessageData() override {
			return vector_;
		}

	private:
		unsigned int vector_;
	};
}

MsiPin *allocateApicMsi(frg::string<KernelAlloc> name) {
	auto guard = frg::guard(&globalIrqSlotsLock);

	int slotIndex = -1;
	for(int i = 0; i < 64; i++) {
		if(!globalIrqSlots[i]->isAvailable())
			continue;
		slotIndex = i;
		break;
	}
	if(slotIndex == -1)
		return nullptr;

	// Create an IRQ pin for the MSI.
	auto pin = frg::construct<ApicMsiPin>(*kernelAlloc,
			std::move(name), 64 + slotIndex);
	pin->configure(IrqConfiguration{
		.trigger = TriggerMode::edge,
		.polarity = Polarity::high
	});

	infoLogger() << "thor: Allocating IRQ slot " << slotIndex
			<< " to " << pin->name() << frg::endlog;
	globalIrqSlots[slotIndex]->link(pin);

	return pin;
}

// --------------------------------------------------------
// I/O APIC management
// --------------------------------------------------------

inline constexpr arch::scalar_register<uint32_t> apicIndex(0x00);
inline constexpr arch::scalar_register<uint32_t> apicData(0x10);

namespace pin_word1 {
	inline constexpr arch::field<uint32_t, unsigned int> vector(0, 8);
	inline constexpr arch::field<uint32_t, unsigned int> deliveryMode(8, 3);
//	inline constexpr arch::field<uint32_t, bool> logicalMode(11, 1);
	inline constexpr arch::field<uint32_t, bool> deliveryStatus(12, 1);
	inline constexpr arch::field<uint32_t, bool> activeLow(13, 1);
	inline constexpr arch::field<uint32_t, bool> remoteIrr(14, 1);
	inline constexpr arch::field<uint32_t, bool> levelTriggered(15, 1);
	inline constexpr arch::field<uint32_t, bool> masked(16, 1);
};

namespace pin_word2 {
	inline constexpr arch::field<uint32_t, unsigned int> destination(24, 8);
};

namespace {
	enum {
		kIoApicId = 0,
		kIoApicVersion = 1,
		kIoApicInts = 16,
	};

	struct IoApic {
	public:
		struct Pin final : IrqPin {
			Pin(IoApic *chip, unsigned int index);

			void dumpHardwareState() override;
			IrqStrategy program(TriggerMode mode, Polarity polarity) override;
			void mask() override;
			void unmask() override;
			void sendEoi() override;

		private:
			IoApic *_chip;
			unsigned int _index;
			int _vector = -1;

			// The following variables store the current pin configuration.
			bool _levelTriggered;
			bool _activeLow;
		};

		IoApic(int apic_id, arch::mem_space space);

		size_t pinCount();

		IrqPin *accessPin(size_t n);

	private:
		uint32_t _loadRegister(uint32_t index) {
			_space.store(apicIndex, index);
			return _space.load(apicData);
		}

		void _storeRegister(uint32_t index, uint32_t value) {
			_space.store(apicIndex, index);
			_space.store(apicData, value);
		}

		int _apicId;
		arch::mem_space _space;
		size_t _numPins;
		// TODO: Replace by dyn_array?
		Pin **_pins;
	};

	static frg::string<KernelAlloc> buildName(int apic_id, unsigned int index) {
		return frg::string<KernelAlloc>{*kernelAlloc, "io-apic."}
				+ frg::to_allocated_string(*kernelAlloc, apic_id)
				+ frg::string<KernelAlloc>{*kernelAlloc, ":"}
				+ frg::to_allocated_string(*kernelAlloc, index);
	}

	IoApic::Pin::Pin(IoApic *chip, unsigned int index)
	: IrqPin{buildName(chip->_apicId, index)}, _chip{chip}, _index{index} { }

	void IoApic::Pin::dumpHardwareState() {
		infoLogger() << "thor: Local APIC state of vector " << _vector << ":"
				<< " ISR: " << (int)getLocalApicIsr(_vector)
				<< ", TMR: " << (getLocalApicTmr(_vector) ? "level" : "edge")
				<< ", IRR: " << (int)getLocalApicIrr(_vector)
				<< frg::endlog;

		arch::bit_value<uint32_t> word1{_chip->_loadRegister(kIoApicInts + _index * 2)};
		infoLogger() << "thor: Configuration of pin " << name() << ": "
				<< ((word1 & pin_word1::levelTriggered) ? "level" : "edge")
				<< "-triggered, active-"
				<< ((word1 & pin_word1::activeLow) ? "low" : "high")
				<< frg::endlog;
		if(_levelTriggered != (word1 & pin_word1::levelTriggered))
			infoLogger() << "\e[31m" "thor: Trigger mode does not match software state!"
					"\e[39m" << frg::endlog;
		if(_activeLow != (word1 & pin_word1::activeLow))
			infoLogger() << "\e[31m" "thor: Trigger mode does not match software state!"
					"\e[39m" << frg::endlog;
		infoLogger() << "thor: I/O APIC state:"
				<< " mask: " << (int)(word1 & pin_word1::masked)
				<< ", delivery status: " << (int)(word1 & pin_word1::deliveryStatus)
				<< ", remote IRR: " << (int)(word1 & pin_word1::remoteIrr)
				<< frg::endlog;
	}

	IrqStrategy IoApic::Pin::program(TriggerMode mode, Polarity polarity) {
		IrqStrategy strategy;
		if(mode == TriggerMode::edge) {
			_levelTriggered = false;
			strategy = IrqStrategy::justEoi;
		}else{
			assert(mode == TriggerMode::level);
			_levelTriggered = true;
			strategy = IrqStrategy::maskThenEoi;
		}

		if(polarity == Polarity::high) {
			_activeLow = false;
		}else{
			assert(polarity == Polarity::low);
			_activeLow = true;
		}

		// Allocate an IRQ vector for the I/O APIC pin.
		if(_vector == -1) {
			auto guard = frg::guard(&globalIrqSlotsLock);

			for(int i = 0; i < 64; i++) {
				if(!globalIrqSlots[i]->isAvailable())
					continue;
				infoLogger() << "thor: Allocating IRQ slot " << i
						<< " to " << name() << frg::endlog;
				globalIrqSlots[i]->link(this);
				_vector = 64 + i;
				break;
			}
		}
		if(_vector == -1)
			panicLogger() << "thor: Could not allocate interrupt vector for "
					<< name() << frg::endlog;

		_chip->_storeRegister(kIoApicInts + _index * 2 + 1,
				static_cast<uint32_t>(pin_word2::destination(0)));
		_chip->_storeRegister(kIoApicInts + _index * 2,
				static_cast<uint32_t>(pin_word1::vector(_vector)
				| pin_word1::deliveryMode(0) | pin_word1::levelTriggered(_levelTriggered)
				| pin_word1::activeLow(_activeLow)));
		return strategy;
	}

	void IoApic::Pin::mask() {
//		infoLogger() << "thor: Masking pin " << _index << frg::endlog;
		_chip->_storeRegister(kIoApicInts + _index * 2,
				static_cast<uint32_t>(pin_word1::vector(_vector)
				| pin_word1::deliveryMode(0) | pin_word1::levelTriggered(_levelTriggered)
				| pin_word1::activeLow(_activeLow) | pin_word1::masked(true)));

		// Dummy load from the I/O APIC to ensure that the mask has taken effect.
		// Without this, we encounter innocuous but annoying races on some hardware:
		// since (x2)APIC EOIs are not necessarily serializing, we observe that the
		// I/O APIC submits IRQs to the local APIC even *after* they have been masked
		// in the I/O APIC.
		_chip->_loadRegister(kIoApicInts + _index * 2);
	}

	void IoApic::Pin::unmask() {
//		infoLogger() << "thor: Unmasking pin " << _index << frg::endlog;
		_chip->_storeRegister(kIoApicInts + _index * 2,
				static_cast<uint32_t>(pin_word1::vector(_vector)
				| pin_word1::deliveryMode(0) | pin_word1::levelTriggered(_levelTriggered)
				| pin_word1::activeLow(_activeLow)));
	}

	void IoApic::Pin::sendEoi() {
		acknowledgeIrq(0);
	}

	IoApic::IoApic(int apic_id, arch::mem_space space)
	: _apicId(apic_id), _space{std::move(space)} {
		_numPins = ((_loadRegister(kIoApicVersion) >> 16) & 0xFF) + 1;
		infoLogger() << "thor: I/O APIC " << apic_id << " supports "
				<< _numPins << " pins" << frg::endlog;

		_pins = frg::construct_n<Pin *>(*kernelAlloc, _numPins);
		for(size_t i = 0; i < _numPins; i++) {
			_pins[i] = frg::construct<Pin>(*kernelAlloc, this, i);

			// Dump interesting configurations.
			arch::bit_value<uint32_t> current{_loadRegister(kIoApicInts + i * 2)};
			if(!(current & pin_word1::masked))
				infoLogger() << "    Pin " << i << " was not masked by BIOS."
						<< frg::endlog;

			// Mask all interrupts before they are configured.
			_storeRegister(kIoApicInts + i * 2, static_cast<uint32_t>(pin_word1::masked(true)));
		}
	}

	size_t IoApic::pinCount() {
		return _numPins;
	}

	IrqPin *IoApic::accessPin(size_t n) {
		return _pins[n];
	}
}

void setupIoApic(int apic_id, int gsi_base, PhysicalAddr address) {
	// TODO: We really only need a single page.
	auto register_ptr = KernelVirtualMemory::global().allocate(0x10000);
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), address,
			page_access::write, CachingMode::null);

	picModel = kModelApic;

	auto apic = frg::construct<IoApic>(*kernelAlloc, apic_id, arch::mem_space{register_ptr});
	for(size_t i = 0; i < apic->pinCount(); i++) {
		auto pin = apic->accessPin(i);
		globalSystemIrqs[gsi_base + i] = pin;
	}

	KernelFiber::run([=] {
		while(true) {
			for(size_t i = 0; i < apic->pinCount(); ++i)
				apic->accessPin(i)->warnIfPending();

			KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(500'000'000));
		}
	});
}

// --------------------------------------------------------
// Legacy PIC management
// --------------------------------------------------------

static initgraph::Task setupPicTask{&globalInitEngine, "x86.setup-legacy-pic",
	initgraph::Entails{getTaskingAvailableStage()},
	[] {
		// TODO: managarm crashes on bochs if we do not remap the legacy PIC.
		// we need to debug that and find the cause of this problem.
		remapLegacyPic(32);
		maskLegacyPic();
	}
};

void ioWait() { }

enum LegacyPicRegisters {
	kPic1Command = 0x20,
	kPic1Data = 0x21,
	kPic2Command = 0xA0,
	kPic2Data = 0xA1
};

enum LegacyPicFlags {
	kIcw1Icw4 = 0x01,
	kIcw1Single = 0x02,
	kIcw1Interval4 = 0x04,
	kIcw1Level = 0x08,
	kIcw1Init = 0x10,

	kIcw4Mode8086 = 0x01,
	kIcw4Auto = 0x02,
	kIcw4BufSlave = 0x08,
	kIcw4BufMaster = 0x0C,
	kIcw4Sfnm = 0x10,

	kOcw3ReadIsr = 0x0B,

	kPicEoi = 0x20
};

void remapLegacyPic(int offset) {
	// save masks
	uint8_t a1 = common::x86::ioInByte(kPic1Data);
	uint8_t a2 = common::x86::ioInByte(kPic2Data);

	// start initilization
	common::x86::ioOutByte(kPic1Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	common::x86::ioOutByte(kPic2Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	common::x86::ioOutByte(kPic1Data, offset);
	ioWait();
	common::x86::ioOutByte(kPic2Data, offset + 8);
	ioWait();

	// setup cascade
	common::x86::ioOutByte(kPic1Data, 4);
	ioWait();
	common::x86::ioOutByte(kPic2Data, 2);
	ioWait();

	common::x86::ioOutByte(kPic1Data, kIcw4Mode8086);
	ioWait();
	common::x86::ioOutByte(kPic2Data, kIcw4Mode8086);
	ioWait();

	// restore saved masks
	common::x86::ioOutByte(kPic1Data, a1);
	common::x86::ioOutByte(kPic2Data, a2);
}

void maskLegacyPic() {
	common::x86::ioOutByte(kPic1Data, 0xFF);
	common::x86::ioOutByte(kPic2Data, 0xFF);
}

bool checkLegacyPicIsr(int irq) {
	if(irq < 8) {
		common::x86::ioOutByte(kPic1Command, kOcw3ReadIsr);
		auto isr = common::x86::ioInByte(kPic1Command);
		return isr & (1 << irq);
	}else{
		assert(irq < 16);
		common::x86::ioOutByte(kPic2Command, kOcw3ReadIsr);
		auto isr = common::x86::ioInByte(kPic2Command);
		return isr & (1 << (irq - 8));
	}
}

// --------------------------------------------------------
// General functions
// --------------------------------------------------------

// TODO: Split this function in two: One for the legacy PIC and one for APIC.
void acknowledgeIrq(int irq) {
	if(picModel == kModelApic) {
		picBase.store(lApicEoi, 0);
	}else if(picModel == kModelLegacy) {
		if(irq >= 8)
			common::x86::ioOutByte(kPic2Command, kPicEoi);
		common::x86::ioOutByte(kPic1Command, kPicEoi);
	}else{
		assert(!"Illegal PIC model");
	}
}

} // namespace thor
