
#include <frg/intrusive.hpp>

#include <arch/bits.hpp>
#include <arch/register.hpp>
#include <arch/mem_space.hpp>
#include <arch/io_space.hpp>

#include "generic/kernel.hpp"

namespace thor {

arch::bit_register<uint64_t> genCapsAndId(0);
arch::bit_register<uint64_t> genConfig(16);
arch::scalar_register<uint64_t> mainCounter(240);
arch::bit_register<uint64_t> timerConfig0(256);
arch::scalar_register<uint64_t> timerComparator0(264);

// genCapsAndId register.
arch::field<uint64_t, bool> has64BitCounter(13, 1);
arch::field<uint64_t, bool> supportsLegacyIrqs(15, 1);
arch::field<uint64_t, uint32_t> counterPeriod(32, 32);

// genConfig register
arch::field<uint64_t, bool> enableCounter(0, 1);
arch::field<uint64_t, bool> enableLegacyIrqs(1, 1);

// timerConfig registers
namespace timer_bits {
	arch::field<uint64_t, bool> enableInt(2, 1);
	arch::field<uint64_t, bool> has64BitComparator(5, 1);
	arch::field<uint64_t, unsigned int> activeIrq(9, 5);
	arch::field<uint64_t, bool> fsbEnabled(14, 1);
	arch::field<uint64_t, bool> fsbCapable(15, 1);
	arch::field<uint64_t, unsigned int> possibleIrqs(32, 32);
};

enum : uint64_t {
	kFemtosPerNano = 1000000,
	kFemtosPerMicro = kFemtosPerNano * 1000,
	kFemtosPerMilli = kFemtosPerMicro * 1000,
	kFemtosPerSecond = kFemtosPerMilli * 1000
};

arch::mem_space hpetBase;
uint64_t hpetFrequency;
bool hpetAvailable;

arch::scalar_register<uint8_t> channel0(64);
arch::bit_register<uint8_t> command(67);

arch::field<uint8_t, int> operatingMode(1, 3);
arch::field<uint8_t, int> accessMode(4, 2);

struct CompareTimer {
	bool operator() (const Timer *a, const Timer *b) const {
		return a->deadline > b->deadline;
	}
};

typedef frg::pairing_heap<
	Timer,
	frg::locate_member<
		Timer,
		frg::pairing_heap_hook<Timer>,
		&Timer::hook
	>,
	CompareTimer
> TimerQueue;

frigg::LazyInitializer<TimerQueue> timerQueue;

struct HpetDevice : IrqSink {
	IrqStatus raise() override {
		auto current = hpetBase.load(mainCounter);
		while(!timerQueue->empty() && timerQueue->top()->deadline < current) {
			auto timer = timerQueue->top();
			timerQueue->pop();
			timer->callback();
		}

		if(!timerQueue->empty())
			hpetBase.store(timerComparator0, timerQueue->top()->deadline);

		// TODO: For edge-triggered mode this is correct (and the IRQ cannot be shared).
		// For level-triggered mode we need to inspect the ISR.
		return irq_status::handled;
	}
};

bool haveTimer() {
	return hpetAvailable;
}

void setupHpet(PhysicalAddr address) {
	frigg::infoLogger() << "HPET at " << (void *)address << frigg::endLog;
	
	// TODO: We really only need a single page.
	auto register_ptr = KernelVirtualMemory::global().allocate(0x10000);
	KernelPageSpace::global().mapSingle4k(VirtualAddr(register_ptr), address,
			page_access::write);
	hpetBase = arch::mem_space(register_ptr);

	auto global_caps = hpetBase.load(genCapsAndId);
	if(!(global_caps & has64BitCounter))
		frigg::infoLogger() << "    Counter is only 32-bits!" << frigg::endLog;
	if(global_caps & supportsLegacyIrqs)
		frigg::infoLogger() << "    Supports legacy replacement." << frigg::endLog;

	hpetFrequency = global_caps & counterPeriod;
	frigg::infoLogger() << "    Tick period: " << hpetFrequency
			<< "fs" << frigg::endLog;
	
	auto timer_caps = hpetBase.load(timerConfig0);
	frigg::infoLogger() << "    Possible IRQ mask: "
			<< (timer_caps & timer_bits::possibleIrqs) << frigg::endLog;
	if(timer_caps & timer_bits::fsbCapable)
		frigg::infoLogger() << "    Timer 0 is capable of FSB interrupts." << frigg::endLog;
	
	// TODO: Disable all timers before programming the first one.

	assert(timer_caps & timer_bits::has64BitComparator);
	if(global_caps & supportsLegacyIrqs) {
		// Enable the HPET.
		hpetBase.store(genConfig, enableCounter(true) | enableLegacyIrqs(true));
		
		// Program HPET timer 0 in one-shot mode.
		hpetBase.store(timerConfig0, timer_bits::enableInt(true));
		hpetBase.store(timerComparator0, 0);
	}else{
		assert((timer_caps & timer_bits::possibleIrqs) & (1 << 2));

		// Enable the HPET.
		hpetBase.store(genConfig, enableCounter(true));
		
		// Program HPET timer 0 in one-shot mode.
		hpetBase.store(timerConfig0, timer_bits::enableInt(true) | timer_bits::activeIrq(2));
		hpetBase.store(timerComparator0, 0);
	}

	auto device = frigg::construct<HpetDevice>(*kernelAlloc);
	attachIrq(getGlobalSystemIrq(2), device);
	frigg::infoLogger() << "HPET IrqSink attached" << frigg::endLog;

	hpetAvailable = true;
	
	// TODO: Move this somewhere else.
	// Disable the legacy PIT (i.e. program to one-shot mode).
	arch::global_io.store(command, operatingMode(0) | accessMode(3));
	arch::global_io.store(channel0, 1);
	arch::global_io.store(channel0, 0);

	calibrateApicTimer();
	
	timerQueue.initialize();
}

void pollSleepNano(uint64_t nanotime) {
	uint64_t counter = hpetBase.load(mainCounter);
	uint64_t goal = counter + nanotime * kFemtosPerNano / hpetFrequency;
	while(hpetBase.load(mainCounter) < goal) {
		frigg::pause();
	}
}

uint64_t currentTicks() {
	return hpetBase.load(mainCounter);
}

uint64_t currentNanos() {
	assert(hpetFrequency > kFemtosPerNano);
	return currentTicks() * (hpetFrequency / kFemtosPerNano);
}

uint64_t durationToTicks(uint64_t seconds,
		uint64_t millis, uint64_t micros, uint64_t nanos) {
	return (seconds * kFemtosPerSecond) / hpetFrequency
			+ (millis * kFemtosPerMilli) / hpetFrequency
			+ (micros * kFemtosPerMicro) / hpetFrequency
			+ (nanos * kFemtosPerNano) / hpetFrequency;
}

void installTimer(Timer *timer) {
	// TODO: We have to make this irq- and thread-safe.
	timerQueue->push(timer);

	hpetBase.store(timerComparator0, timerQueue->top()->deadline);
	// TODO: We might have missed the deadline already if it is short enough.
	// Read the counter here and dequeue all elapsed timers manually.
}

} // namespace thor

