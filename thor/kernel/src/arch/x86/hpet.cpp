
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
arch::field<uint64_t, bool> countSizeCap(13, 1);
arch::field<uint64_t, uint32_t> counterClkPeriod(32, 32);

// genConfig register
arch::field<uint64_t, bool> enableCnf(0, 1);

// timerConfig registers
arch::field<uint64_t, bool> tnIntEnbCnf(2, 1);
arch::field<uint64_t, int> tnIntRouteCnf(9, 5);

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

typedef frigg::PriorityQueue<Timer, KernelAlloc> TimerQueue;
frigg::LazyInitializer<TimerQueue> timerQueue;

bool haveTimer() {
	return hpetAvailable;
}

void setupHpet(PhysicalAddr address) {
	frigg::infoLogger() << "HPET at " << (void *)address << frigg::endLog;
	
	hpetBase = arch::mem_space(accessPhysical<uint64_t>(address));

	auto caps = hpetBase.load(genCapsAndId);
	if(!(caps & countSizeCap))
		frigg::infoLogger() << "HPET only has a 32-bit counter" << frigg::endLog;

	hpetFrequency = caps & counterClkPeriod;
	frigg::infoLogger() << "HPET frequency: " << hpetFrequency << frigg::endLog;
	
	auto config = hpetBase.load(genConfig);
	config |= enableCnf(true);
	hpetBase.store(genConfig, config);
	
	frigg::infoLogger() << "Enabled HPET" << frigg::endLog;
	
	// disable the legacy PIT (i.e. program to one-shot mode)
	arch::global_io.store(command, operatingMode(0) | accessMode(3));
	arch::global_io.store(channel0, 1);
	arch::global_io.store(channel0, 0);
	
	// program hpet timer 0 in one-shot mode
	auto timer_config = hpetBase.load(timerConfig0);
	timer_config &= ~tnIntRouteCnf;
	timer_config |= tnIntRouteCnf(2);
	timer_config |= tnIntEnbCnf(true);
	frigg::infoLogger() << static_cast<uint64_t>(timer_config) << frigg::endLog;
	hpetBase.store(timerConfig0, timer_config);
	hpetBase.store(timerComparator0, 0);

	hpetAvailable = true;

	calibrateApicTimer();
	
	timerQueue.initialize(*kernelAlloc);
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

void installTimer(Timer timer) {
	// TODO: We have to make this irq- and thread-safe.
	timerQueue->enqueue(frigg::move(timer));

	hpetBase.store(timerComparator0, timerQueue->front().deadline);
	// TODO: We might have missed the deadline already if it is short enough.
	// Read the counter here and dequeue all elapsed timers manually.
}

void timerInterrupt() {
	auto current = hpetBase.load(mainCounter);
	while(!timerQueue->empty() && timerQueue->front().deadline < current) {
		Timer timer = timerQueue->dequeue();
		timer.callback();
	}

	if(!timerQueue->empty())
		hpetBase.store(timerComparator0, timerQueue->front().deadline);
}

} // namespace thor

