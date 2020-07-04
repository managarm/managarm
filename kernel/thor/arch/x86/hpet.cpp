
#include <frg/intrusive.hpp>
#include <frigg/debug.hpp>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/mem_space.hpp>
#include <arch/io_space.hpp>

#include <thor-internal/arch/hpet.hpp>
#include <thor-internal/arch/paging.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/irq.hpp>

namespace thor {

namespace {
	// For debugging 32-bit counters.
	constexpr bool force32BitHpet = false;
}

inline constexpr arch::bit_register<uint64_t> genCapsAndId(0);
inline constexpr arch::bit_register<uint64_t> genConfig(16);
inline constexpr arch::scalar_register<uint64_t> mainCounter(240);
inline constexpr arch::bit_register<uint64_t> timerConfig0(256);
inline constexpr arch::scalar_register<uint64_t> timerComparator0(264);

// genCapsAndId register.
inline constexpr arch::field<uint64_t, bool> has64BitCounter(13, 1);
inline constexpr arch::field<uint64_t, bool> supportsLegacyIrqs(15, 1);
inline constexpr arch::field<uint64_t, uint32_t> counterPeriod(32, 32);

// genConfig register
inline constexpr arch::field<uint64_t, bool> enableCounter(0, 1);
inline constexpr arch::field<uint64_t, bool> enableLegacyIrqs(1, 1);

// timerConfig registers
namespace timer_bits {
	inline constexpr arch::field<uint64_t, bool> enableInt(2, 1);
	inline constexpr arch::field<uint64_t, bool> has64BitComparator(5, 1);
	inline constexpr arch::field<uint64_t, bool> forceTo32Bit(8, 1);
	inline constexpr arch::field<uint64_t, unsigned int> activeIrq(9, 5);
	inline constexpr arch::field<uint64_t, bool> fsbEnabled(14, 1);
	inline constexpr arch::field<uint64_t, bool> fsbCapable(15, 1);
	inline constexpr arch::field<uint64_t, unsigned int> possibleIrqs(32, 32);
};

enum : uint64_t {
	kFemtosPerNano = 1'000'000,
	kFemtosPerMicro = kFemtosPerNano * 1000,
	kFemtosPerMilli = kFemtosPerMicro * 1000,
	kFemtosPerSecond = kFemtosPerMilli * 1000
};

arch::mem_space hpetBase;
uint64_t hpetPeriod;
bool hpetAvailable;

inline constexpr arch::scalar_register<uint8_t> channel0(64);
inline constexpr arch::bit_register<uint8_t> command(67);

inline constexpr arch::field<uint8_t, int> operatingMode(1, 3);
inline constexpr arch::field<uint8_t, int> accessMode(4, 2);

struct HpetDevice : IrqSink, ClockSource, AlarmTracker {
	friend void setupHpet(PhysicalAddr);

private:
	using Mutex = frigg::TicketLock;

	static constexpr bool logIrqs = false;

public:
	HpetDevice()
	: IrqSink{frigg::String<KernelAlloc>{*kernelAlloc, "hpet-irq"}} { }

	IrqStatus raise() override {
		if(logIrqs)
			frigg::infoLogger() << "hpet: Irq was raised." << frigg::endLog;
//		auto irq_lock = frigg::guard(&irqMutex());
//		auto lock = frigg::guard(&_mutex);

		fireAlarm();

		// TODO: For edge-triggered mode this is correct (and the IRQ cannot be shared).
		// For level-triggered mode we need to inspect the ISR.
		if(logIrqs)
			frigg::infoLogger() << "hpet: Handler completed." << frigg::endLog;
		return IrqStatus::acked;
	}

public:
	uint64_t currentNanos() override {
		// TODO: Return a correct value even if the main counter overflows.
		//       Use one of the comparators to track the number of overflows.
		assert(hpetPeriod > kFemtosPerNano);
		return hpetBase.load(mainCounter) * (hpetPeriod / kFemtosPerNano);
	}

public:
	void arm(uint64_t nanos) override {
		uint64_t ticks;
		auto now = systemClockSource()->currentNanos();
		if(nanos < now) {
			ticks = 1;
		}else{
			ticks = hpetBase.load(mainCounter) + (nanos - now) / (hpetPeriod / kFemtosPerNano);
		}
		if(!_comparatorIs64Bit) {
			// TODO: We could certainly do something better here.
			//       - If the tick happens during the next main counter cycle (despite overflow),
			//         everything works as expected; we do not need to warn.
			//       - Adjust this code one we count the number of overflows.
			if(ticks & ~uint64_t{0xFFFFFFFF})
				frigg::infoLogger() << "\e[31m" "thor: HPET comparator overflow" "\e[39m"
						<< frigg::endLog;
			ticks &= ~uint64_t(0xFFFFFFFF);
		}
		hpetBase.store(timerComparator0, ticks);
	}

private:
//	Mutex _mutex;
	bool _comparatorIs64Bit = true;
};

frigg::LazyInitializer<HpetDevice> hpetDevice;
ClockSource *hpetClockSource;
AlarmTracker *hpetAlarmTracker;

bool haveTimer() {
	return hpetAvailable;
}

void setupHpet(PhysicalAddr address) {
	frigg::infoLogger() << "HPET at " << (void *)address << frigg::endLog;

	hpetDevice.initialize();

	// TODO: We really only need a single page.
	auto register_ptr = KernelVirtualMemory::global().allocate(0x10000);
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), address,
			page_access::write, CachingMode::null);
	hpetBase = arch::mem_space(register_ptr);

	auto global_caps = hpetBase.load(genCapsAndId);
	if(!(global_caps & has64BitCounter)) {
		frigg::infoLogger() << "    Counter is only 32-bits!" << frigg::endLog;
	}else if(force32BitHpet) {
		frigg::infoLogger() << "    Forcing HPET to use 32-bit mode!" << frigg::endLog;
	}
	if(global_caps & supportsLegacyIrqs)
		frigg::infoLogger() << "    Supports legacy replacement." << frigg::endLog;

	hpetPeriod = global_caps & counterPeriod;
	frigg::infoLogger() << "    Tick period: " << hpetPeriod
			<< "fs" << frigg::endLog;

	auto timer_caps = hpetBase.load(timerConfig0);
	frigg::infoLogger() << "    Possible IRQ mask: "
			<< (timer_caps & timer_bits::possibleIrqs) << frigg::endLog;
	if(timer_caps & timer_bits::fsbCapable)
		frigg::infoLogger() << "    Timer 0 is capable of FSB interrupts." << frigg::endLog;

	// TODO: Disable all timers before programming the first one.
	hpetBase.store(timerConfig0, timer_bits::enableInt(false));

	// Enable the HPET counter.
	if(!(timer_caps & timer_bits::has64BitComparator) || force32BitHpet)
		hpetDevice->_comparatorIs64Bit = false;
	if(global_caps & supportsLegacyIrqs) {
		hpetBase.store(genConfig, enableCounter(true) | enableLegacyIrqs(true));
	}else{
		hpetBase.store(genConfig, enableCounter(true));
	}
	
//	IrqPin::attachSink(getGlobalSystemIrq(2), hpetDevice.get());

	// Program HPET timer 0 in one-shot mode.
	if(global_caps & supportsLegacyIrqs) {
		hpetBase.store(timerConfig0, timer_bits::forceTo32Bit(!hpetDevice->_comparatorIs64Bit)
				| timer_bits::enableInt(false));
		hpetBase.store(timerComparator0, 0);
		hpetBase.store(timerConfig0, timer_bits::enableInt(true));
	}else{
		assert((timer_caps & timer_bits::possibleIrqs) & (1 << 2));
		hpetBase.store(timerConfig0, timer_bits::forceTo32Bit(!hpetDevice->_comparatorIs64Bit)
				| timer_bits::enableInt(false) | timer_bits::activeIrq(2));
		hpetBase.store(timerComparator0, 0);
		hpetBase.store(timerConfig0, timer_bits::enableInt(true) | timer_bits::activeIrq(2));
	}

	hpetClockSource = hpetDevice.get();
	hpetAlarmTracker = hpetDevice.get();
	hpetAvailable = true;
	
	// TODO: Move this somewhere else.
	// Disable the legacy PIT (i.e. program to one-shot mode).
	//arch::global_io.store(command, operatingMode(0) | accessMode(3));
	//arch::global_io.store(channel0, 1);
	//arch::global_io.store(channel0, 0);

	calibrateApicTimer();
}

void pollSleepNano(uint64_t nanotime) {
	uint64_t counter = hpetBase.load(mainCounter);
	uint64_t goal = counter + nanotime * kFemtosPerNano / hpetPeriod;
	while(hpetBase.load(mainCounter) < goal) {
		frigg::pause();
	}
}

} // namespace thor

