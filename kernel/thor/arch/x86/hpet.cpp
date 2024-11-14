#include <arch/bits.hpp>
#include <arch/io_space.hpp>
#include <arch/mem_space.hpp>
#include <arch/register.hpp>
#include <frg/intrusive.hpp>
#include <uacpi/acpi.h>
#include <uacpi/tables.h>
#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch/hpet.hpp>
#include <thor-internal/arch/paging.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/main.hpp>
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

struct HpetDevice final : IrqSink, ClockSource, AlarmTracker {
	friend void setupHpet(PhysicalAddr);

private:
	using Mutex = frg::ticket_spinlock;

	static constexpr bool logIrqs = false;

public:
	HpetDevice()
	: IrqSink{frg::string<KernelAlloc>{*kernelAlloc, "hpet-irq"}} { }

	IrqStatus raise() override {
		if(logIrqs)
			infoLogger() << "hpet: Irq was raised." << frg::endlog;
//		auto irq_lock = frg::guard(&irqMutex());
//		auto lock = frg::guard(&_mutex);

		fireAlarm();

		// TODO: For edge-triggered mode this is correct (and the IRQ cannot be shared).
		// For level-triggered mode we need to inspect the ISR.
		if(logIrqs)
			infoLogger() << "hpet: Handler completed." << frg::endlog;
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
				urgentLogger() << "thor: HPET comparator overflow" << frg::endlog;
			ticks &= ~uint64_t(0xFFFFFFFF);
		}
		hpetBase.store(timerComparator0, ticks);
	}

private:
//	Mutex _mutex;
	bool _comparatorIs64Bit = true;
};

frg::manual_box<HpetDevice> hpetDevice;
ClockSource *hpetClockSource;
AlarmTracker *hpetAlarmTracker;

bool haveTimer() {
	return hpetAvailable;
}

void setupHpet(PhysicalAddr address) {
	infoLogger() << "HPET at " << (void *)address << frg::endlog;

	hpetDevice.initialize();

	// TODO: We really only need a single page.
	auto register_ptr = KernelVirtualMemory::global().allocate(0x10000);
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), address,
			page_access::write, CachingMode::null);
	hpetBase = arch::mem_space(register_ptr);

	auto global_caps = hpetBase.load(genCapsAndId);
	if(!(global_caps & has64BitCounter)) {
		infoLogger() << "    Counter is only 32-bits!" << frg::endlog;
	}else if(force32BitHpet) {
		infoLogger() << "    Forcing HPET to use 32-bit mode!" << frg::endlog;
	}
	if(global_caps & supportsLegacyIrqs)
		infoLogger() << "    Supports legacy replacement." << frg::endlog;

	hpetPeriod = global_caps & counterPeriod;
	infoLogger() << "    Tick period: " << hpetPeriod
			<< "fs" << frg::endlog;

	auto timer_caps = hpetBase.load(timerConfig0);
	infoLogger() << "    Possible IRQ mask: "
			<< (timer_caps & timer_bits::possibleIrqs) << frg::endlog;
	if(timer_caps & timer_bits::fsbCapable)
		infoLogger() << "    Timer 0 is capable of FSB interrupts." << frg::endlog;

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
}

void pollSleepNano(uint64_t nanotime) {
	uint64_t counter = hpetBase.load(mainCounter);
	uint64_t goal = counter + nanotime * kFemtosPerNano / hpetPeriod;
	while(hpetBase.load(mainCounter) < goal) {
		pause();
	}
}

struct HpetEntry {
	uint32_t generalCapsAndId;
	acpi_gas address;
	uint8_t hpetNumber;
	uint16_t minimumTick;
	uint8_t pageProtection;
} __attribute__ (( packed ));

initgraph::Stage *getHpetInitializedStage() {
	static initgraph::Stage s{&globalInitEngine, "x86.hpet-initialized"};
	return &s;
}

static initgraph::Task initHpetTask{&globalInitEngine, "x86.init-hpet",
	initgraph::Requires{getApicDiscoveryStage(), // For APIC calibration.
			acpi::getTablesDiscoveredStage()},
	initgraph::Entails{getHpetInitializedStage()},
	// Initialize the HPET.
	[] {
		uacpi_table hpetTbl;

		auto ret = uacpi_table_find_by_signature("HPET", &hpetTbl);
		if(ret != UACPI_STATUS_OK) {
			urgentLogger() << "thor: No HPET table!" << frg::endlog;
			return;
		}
		if(hpetTbl.hdr->length < sizeof(acpi_sdt_hdr) + sizeof(HpetEntry)) {
			urgentLogger() << "thor: HPET table has no entries!" << frg::endlog;
			return;
		}
		auto hpetEntry = (HpetEntry *)(hpetTbl.virt_addr + sizeof(acpi_sdt_hdr));
		infoLogger() << "thor: Setting up HPET" << frg::endlog;
		assert(hpetEntry->address.address_space_id == ACPI_AS_ID_SYS_MEM);
		setupHpet(hpetEntry->address.address);
	}
};

} // namespace thor

