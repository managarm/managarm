
#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/mem_space.hpp>
#include <arch/io_space.hpp>

#include "generic/fiber.hpp"
#include "generic/kernel.hpp"
#include "generic/irq.hpp"
#include "generic/service_helpers.hpp"

namespace thor {

extern frigg::LazyInitializer<frigg::Vector<KernelFiber *, KernelAlloc>> earlyFibers;

inline constexpr arch::bit_register<uint32_t> lApicId(0x0020);
inline constexpr arch::scalar_register<uint32_t> lApicEoi(0x00B0);
inline constexpr arch::bit_register<uint32_t> lApicSpurious(0x00F0);
inline constexpr arch::bit_register<uint32_t> lApicIcrLow(0x0300);
inline constexpr arch::bit_register<uint32_t> lApicIcrHigh(0x0310);
inline constexpr arch::bit_register<uint32_t> lApicLvtTimer(0x0320);
inline constexpr arch::bit_register<uint32_t> lApicLvtPerfCount(0x0340);
inline constexpr arch::bit_register<uint32_t> lApicLvtLocal0(0x0350);
inline constexpr arch::bit_register<uint32_t> lApicLvtLocal1(0x0360);
inline constexpr arch::scalar_register<uint32_t> lApicInitCount(0x0380);
inline constexpr arch::scalar_register<uint32_t> lApicCurCount(0x0390);

// lApicId registers
inline constexpr arch::field<uint32_t, uint8_t> apicId(24, 8);

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

// lApicLvtTimer registers
inline constexpr arch::field<uint32_t, uint8_t> apicLvtVector(0, 8);
inline constexpr arch::field<uint32_t, bool> apicLvtMask(16, 1);
inline constexpr arch::field<uint32_t, uint8_t> apicLvtMode(8, 3);

arch::mem_space picBase;

enum {
	kModelLegacy = 1,
	kModelApic = 2
};

static int picModel = kModelLegacy;

// --------------------------------------------------------
// Local APIC timer
// --------------------------------------------------------

// TODO: APIC variables should be CPU-specific.
uint32_t apicTicksPerMilli;
namespace {
	GlobalApicContext *globalApicContextInstance;

	LocalApicContext *localApicContext() {
		return &getCpuData()->apicContext;
	}
}

GlobalApicContext *globalApicContext() {
	return globalApicContextInstance;
}

void GlobalApicContext::GlobalAlarmSlot::arm(uint64_t nanos) {
	assert(apicTicksPerMilli > 0);

	{
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&globalApicContext()->_mutex);
		globalApicContext()->_globalDeadline = nanos;
	}
	LocalApicContext::_updateLocalTimer();
}

LocalApicContext::LocalApicContext()
: _preemptionDeadline{0}, _globalDeadline{0} { }

void LocalApicContext::setPreemption(uint64_t nanos) {
	assert(apicTicksPerMilli > 0);
	
	localApicContext()->_preemptionDeadline = nanos;
	LocalApicContext::_updateLocalTimer();
}

void LocalApicContext::handleTimerIrq() {
//	frigg::infoLogger() << "thor [CPU " << getLocalApicId() << "]: Timer IRQ triggered"
//			<< frigg::endLog;
	auto self = localApicContext();
	auto now = systemClockSource()->currentNanos();

	if(self->_preemptionDeadline && now > self->_preemptionDeadline)
		self->_preemptionDeadline = 0;

	if(self->_globalDeadline && now > self->_globalDeadline) {
		self->_globalDeadline = 0;
		globalApicContext()->_globalAlarmInstance.fireAlarm();

		// Update the global deadline to avoid calling fireAlarm() on the next IRQ.
		{
			auto irq_lock = frigg::guard(&irqMutex());
			auto lock = frigg::guard(&globalApicContext()->_mutex);
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
		auto irq_lock = frigg::guard(&irqMutex());
		auto lock = frigg::guard(&globalApicContext()->_mutex);
		localApicContext()->_globalDeadline = globalApicContext()->_globalDeadline;
	}

	consider(localApicContext()->_preemptionDeadline);
	consider(localApicContext()->_globalDeadline);
	
	if(!deadline) {
		picBase.store(lApicInitCount, 0);
		return;
	}

	auto now = systemClockSource()->currentNanos();
	uint64_t ticks;
	if(deadline < now) {
//		frigg::infoLogger() << "thor [CPU " << getLocalApicId() << "]: Setting single tick timer"
//				<< frigg::endLog;
		ticks = 1;
	}else{
//		frigg::infoLogger() << "thor [CPU " << getLocalApicId() << "]: Setting timer "
//				<< ((deadline - now)/1000) << " us in the future" << frigg::endLog;
		auto of = __builtin_mul_overflow(deadline - now, apicTicksPerMilli, &ticks);
		assert(!of);
		ticks /= 1'000'000;
		if(!ticks)
			ticks = 1;
	}
	picBase.store(lApicInitCount, ticks);
}

void armPreemption(uint64_t nanos) {
	LocalApicContext::setPreemption(systemClockSource()->currentNanos() + nanos);
}

void disarmPreemption() {
	LocalApicContext::setPreemption(0);
}

// --------------------------------------------------------
// Local PIC management
// --------------------------------------------------------

void initLocalApicOnTheSystem() {
	uint64_t msr = frigg::arch_x86::rdmsr(frigg::arch_x86::kMsrLocalApicBase);
	assert(msr & (1 << 11)); // local APIC is enabled

	// TODO: We really only need a single page.
	auto register_ptr = KernelVirtualMemory::global().allocate(0x10000);
	// TODO: Intel SDM specifies that we should mask out all
	// bits > the physical address limit of the msr.
	// For now we just assume that they are zero.
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), msr & ~PhysicalAddr{0xFFF},
			page_access::write, CachingMode::null);
	picBase = arch::mem_space(register_ptr);

	frigg::infoLogger() << "Booting on CPU #" << getLocalApicId() << frigg::endLog;
}

void initLocalApicPerCpu() {
	auto dumpLocalInt = [&] (int index) {
		auto regstr = (index == 0 ? lApicLvtLocal0 : lApicLvtLocal1);
		auto lvt = picBase.load(regstr);
		frigg::infoLogger() << "thor: CPU #" << getLocalApicId()
				<< " LINT " << index << " mode is " << (lvt & apicLvtMode)
				<< ", it is " << ((lvt & apicLvtMask) ? "masked" : "not masked")
				<< frigg::endLog;
	};

	// Enable the local APIC.
	uint32_t spurious_vector = 0x81;
	picBase.store(lApicSpurious, apicSpuriousVector(spurious_vector)
			| apicSpuriousSwEnable(true));
	
	dumpLocalInt(0);
	dumpLocalInt(1);
	
	// Setup a timer interrupt for scheduling.
	picBase.store(lApicLvtTimer, apicLvtVector(0xFF));

	// Setup the PMI.
	picBase.store(lApicLvtPerfCount, apicLvtMode(4));
}

uint32_t getLocalApicId() {
	return picBase.load(lApicId) & apicId;
}

uint64_t localTicks() {
	return picBase.load(lApicCurCount);
}

uint64_t rdtsc() {
	uint32_t lsw, msw;
	asm volatile ("rdtsc" : "=a"(lsw), "=d"(msw));
	return (static_cast<uint64_t>(msw) << 32)
			| static_cast<uint64_t>(lsw);
}

uint64_t tscTicksPerMilli;

struct TimeStampCounter : ClockSource {
	uint64_t currentNanos() override {
		auto r = rdtsc() * 1'000'000 / tscTicksPerMilli;
//		frigg::infoLogger() << r << frigg::endLog;
		return r;
	}
};

TimeStampCounter *globalTscInstance;

extern ClockSource *hpetClockSource;
extern AlarmTracker *hpetAlarmTracker;
extern ClockSource *globalClockSource;
extern PrecisionTimerEngine *globalTimerEngine;

void calibrateApicTimer() {
	const uint64_t millis = 100;

	picBase.store(lApicInitCount, 0xFFFFFFFF);
	pollSleepNano(millis * 1'000'000);
	uint32_t elapsed = 0xFFFFFFFF
			- picBase.load(lApicCurCount);
	picBase.store(lApicInitCount, 0);

	apicTicksPerMilli = elapsed / millis;
	frigg::infoLogger() << "thor: Local APIC ticks/ms: " << apicTicksPerMilli << frigg::endLog;
	
	auto tsc_start = rdtsc();
	pollSleepNano(millis * 1'000'000);
	auto tsc_elapsed = rdtsc() - tsc_start;
	
	tscTicksPerMilli = tsc_elapsed / millis;
	frigg::infoLogger() << "thor: TSC ticks/ms: " << tscTicksPerMilli << frigg::endLog;

	globalTscInstance = frigg::construct<TimeStampCounter>(*kernelAlloc);
	globalApicContextInstance = frigg::construct<GlobalApicContext>(*kernelAlloc);

	globalClockSource = globalTscInstance;
//	globalClockSource = hpetClockSource;
	globalTimerEngine = frigg::construct<PrecisionTimerEngine>(*kernelAlloc,
			globalClockSource, globalApicContext()->globalAlarm());
//			globalClockSource, hpetAlarmTracker);
}

void acknowledgeIpi() {
	picBase.store(lApicEoi, 0);
}

void raiseInitAssertIpi(uint32_t dest_apic_id) {
	picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
	// DM:init = 5, Level:assert = 1, TM:Level = 1
	picBase.store(lApicIcrLow, apicIcrLowDelivMode(5)
			| apicIcrLowLevel(true) | apicIcrLowTriggerMode(true));
	while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
		// Wait for IPI delivery.
	}
}

void raiseInitDeassertIpi(uint32_t dest_apic_id) {
	picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
	// DM:init = 5, TM:Level = 1
	picBase.store(lApicIcrLow, apicIcrLowDelivMode(5)
			| apicIcrLowTriggerMode(true));
	while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
		// Wait for IPI delivery.
	}
}

void raiseStartupIpi(uint32_t dest_apic_id, uint32_t page) {
	assert((page % 0x1000) == 0);
	uint32_t vector = page / 0x1000; // determines the startup code page
	picBase.store(lApicIcrHigh, apicIcrHighDestField(dest_apic_id));
	// DM:startup = 6
	picBase.store(lApicIcrLow, apicIcrLowVector(vector)
			| apicIcrLowDelivMode(6));
	while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
		// Wait for IPI delivery.
	}
}

void sendShootdownIpi() {
	picBase.store(lApicIcrHigh, apicIcrHighDestField(0));
	picBase.store(lApicIcrLow, apicIcrLowVector(0xF0) | apicIcrLowDelivMode(0)
			| apicIcrLowLevel(true) | apicIcrLowShorthand(2));
	while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
		// Wait for IPI delivery.
	}
}

void sendPingIpi(uint32_t apic) {
//	frigg::infoLogger() << "thor [CPU" << getLocalApicId() << "]: Sending ping" << frigg::endLog;
	picBase.store(lApicIcrHigh, apicIcrHighDestField(apic));
	picBase.store(lApicIcrLow, apicIcrLowVector(0xF1) | apicIcrLowDelivMode(0)
			| apicIcrLowLevel(true) | apicIcrLowShorthand(0));
	while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
		// Wait for IPI delivery.
	}
}

void sendGlobalNmi() {
	// Send the NMI to all /other/ CPUs but not to the current one.
	picBase.store(lApicIcrHigh, apicIcrHighDestField(0));
	picBase.store(lApicIcrLow, apicIcrLowVector(0) | apicIcrLowDelivMode(4)
			| apicIcrLowLevel(true) | apicIcrLowShorthand(3));
	while(picBase.load(lApicIcrLow) & apicIcrLowDelivStatus) {
		// Wait for IPI delivery.
	}
}

// --------------------------------------------------------

IrqPin *globalSystemIrqs[256];

IrqPin *getGlobalSystemIrq(size_t n) {
	assert(n <= 256);
	return globalSystemIrqs[n];
}

// --------------------------------------------------------
// I/O APIC management
// --------------------------------------------------------

// TODO: Replace this by proper IRQ allocation.
extern frigg::LazyInitializer<IrqSlot> globalIrqSlots[64];

inline constexpr arch::scalar_register<uint32_t> apicIndex(0x00);
inline constexpr arch::scalar_register<uint32_t> apicData(0x10);

namespace pin_word1 {
	inline constexpr arch::field<uint32_t, unsigned int> vector(0, 8);
	inline constexpr arch::field<uint32_t, unsigned int> deliveryMode(8, 3);
//	inline constexpr arch::field<uint32_t, bool> logicalMode(11, 1);
//	inline constexpr arch::field<uint32_t, bool> deliveryStatus(12, 1);
	inline constexpr arch::field<uint32_t, bool> activeLow(13, 1);
//	inline constexpr arch::field<uint32_t, bool> remotePending(14, 1);
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
		struct Pin : IrqPin {
			Pin(IoApic *chip, unsigned int index);

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

	static frigg::String<KernelAlloc> buildName(int apic_id, unsigned int index) {
		return frigg::String<KernelAlloc>{*kernelAlloc, "io-apic."}
				+ frigg::to_string(*kernelAlloc, apic_id)
				+ frigg::String<KernelAlloc>{*kernelAlloc, ":"}
				+ frigg::to_string(*kernelAlloc, index);
	}

	IoApic::Pin::Pin(IoApic *chip, unsigned int index)
	: IrqPin{buildName(chip->_apicId, index)}, _chip{chip}, _index{index} { }

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
		if(_vector == -1)
			for(int i = 0; i < 64; i++) {
				if(!globalIrqSlots[i]->isAvailable())
					continue;
				frigg::infoLogger() << "thor: Allocating IRQ slot " << i
						<< " to " << name() << frigg::endLog;
				globalIrqSlots[i]->link(this);
				_vector = 64 + i;
				break;
			}
		if(_vector == -1)
			frigg::panicLogger() << "thor: Could not allocate interrupt vector for "
					<< name() << frigg::endLog;

		_chip->_storeRegister(kIoApicInts + _index * 2 + 1,
				static_cast<uint32_t>(pin_word2::destination(0)));
		_chip->_storeRegister(kIoApicInts + _index * 2,
				static_cast<uint32_t>(pin_word1::vector(_vector)
				| pin_word1::deliveryMode(0) | pin_word1::levelTriggered(_levelTriggered)
				| pin_word1::activeLow(_activeLow)));
		return strategy;
	}
	
	void IoApic::Pin::mask() {
//		frigg::infoLogger() << "thor: Masking pin " << _index << frigg::endLog;
		_chip->_storeRegister(kIoApicInts + _index * 2,
				static_cast<uint32_t>(pin_word1::vector(_vector)
				| pin_word1::deliveryMode(0) | pin_word1::levelTriggered(_levelTriggered)
				| pin_word1::activeLow(_activeLow) | pin_word1::masked(true)));
	}

	void IoApic::Pin::unmask() {
//		frigg::infoLogger() << "thor: Unmasking pin " << _index << frigg::endLog;
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
		frigg::infoLogger() << "thor: I/O APIC " << apic_id << " supports "
				<< _numPins << " pins" << frigg::endLog;

		_pins = frigg::constructN<Pin *>(*kernelAlloc, _numPins);
		for(size_t i = 0; i < _numPins; i++) {
			_pins[i] = frigg::construct<Pin>(*kernelAlloc, this, i);

			// Dump interesting configurations.
			arch::bit_value<uint32_t> current{_loadRegister(kIoApicInts + i * 2)};
			if(!(current & pin_word1::masked))
				frigg::infoLogger() << "    Pin " << i << " was not masked by BIOS."
						<< frigg::endLog;

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

	auto apic = frigg::construct<IoApic>(*kernelAlloc, apic_id, arch::mem_space{register_ptr});
	for(size_t i = 0; i < apic->pinCount(); i++) {
		auto pin = apic->accessPin(i);
		globalSystemIrqs[gsi_base + i] = pin;
	}

	earlyFibers->push(KernelFiber::post([=] {
		while(true) {
			for(size_t i = 0; i < apic->pinCount(); ++i)
				apic->accessPin(i)->warnIfPending();

			fiberSleep(500'000'000);
		}
	}));
}

// --------------------------------------------------------
// Legacy PIC management
// --------------------------------------------------------

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
	uint8_t a1 = frigg::arch_x86::ioInByte(kPic1Data);
	uint8_t a2 = frigg::arch_x86::ioInByte(kPic2Data);

	// start initilization
	frigg::arch_x86::ioOutByte(kPic1Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Command, kIcw1Init | kIcw1Icw4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic1Data, offset);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, offset + 8);
	ioWait();

	// setup cascade
	frigg::arch_x86::ioOutByte(kPic1Data, 4);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, 2);
	ioWait();

	frigg::arch_x86::ioOutByte(kPic1Data, kIcw4Mode8086);
	ioWait();
	frigg::arch_x86::ioOutByte(kPic2Data, kIcw4Mode8086);
	ioWait();

	// restore saved masks
	frigg::arch_x86::ioOutByte(kPic1Data, a1);
	frigg::arch_x86::ioOutByte(kPic2Data, a2);
}

void setupLegacyPic() {
	remapLegacyPic(32);
}

void maskLegacyPic() {
	frigg::arch_x86::ioOutByte(kPic1Data, 0xFF);
	frigg::arch_x86::ioOutByte(kPic2Data, 0xFF);
}

bool checkLegacyPicIsr(int irq) {
	if(irq < 8) {
		frigg::arch_x86::ioOutByte(kPic1Command, kOcw3ReadIsr);
		auto isr = frigg::arch_x86::ioInByte(kPic1Command);
		return isr & (1 << irq);
	}else{
		assert(irq < 16);
		frigg::arch_x86::ioOutByte(kPic2Command, kOcw3ReadIsr);
		auto isr = frigg::arch_x86::ioInByte(kPic2Command);
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
			frigg::arch_x86::ioOutByte(kPic2Command, kPicEoi);
		frigg::arch_x86::ioOutByte(kPic1Command, kPicEoi);
	}else{
		assert(!"Illegal PIC model");
	}
}

} // namespace thor

