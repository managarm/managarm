
#include "../kernel.hpp"

namespace thor {

enum {
	kHpetGenCapsAndId = 0,
	kHpetGenConfig = 2,
	kHpetGenIntStatus = 4,
	kHpetMainCounter = 30,
	kHpetTimerConfig0 = 32,
	kHpetTimerComparator0 = 33,
};

enum {
	// kHpetCapsAndId register
	kHpet64BitCounter = 0x2000,

	// kHpetGenConfig register
	kHpetEnable = 1,
	
	// TimerConfig registers
	kHpetEnableInt = 4,
	kHpetIntShift = 9
};

enum : uint64_t {
	kFemtosPerNano = 1000000,
	kFemtosPerMicro = kFemtosPerNano * 1000,
	kFemtosPerMilli = kFemtosPerMicro * 1000,
	kFemtosPerSecond = kFemtosPerMilli * 1000
};

uint64_t *hpetRegs;
uint64_t hpetFrequency;

enum {
	kPitChannel0 = 0x40,
	kPitCommand = 0x43,
};

enum {
	kPitOnTerminalCount = 0x00,
	kPitRateGenerator = 0x04,
	kPitLowHigh = 0x30
};

typedef frigg::PriorityQueue<Timer, KernelAlloc> TimerQueue;
frigg::LazyInitializer<TimerQueue> timerQueue;

void setupHpet(PhysicalAddr address) {
	infoLogger->log() << "HPET at " << (void *)address << frigg::EndLog();
	hpetRegs = accessPhysical<uint64_t>(address);

	uint64_t caps = frigg::volatileRead<uint64_t>(&hpetRegs[kHpetGenCapsAndId]);
	if((caps & kHpet64BitCounter) == 0)
		infoLogger->log() << "HPET only has a 32-bit counter" << frigg::EndLog();
	if((caps & kHpet64BitCounter) == 0)
		infoLogger->log() << "HPET only has a 32-bit counter" << frigg::EndLog();

	hpetFrequency = caps >> 32;
	infoLogger->log() << "HPET frequency: " << hpetFrequency << frigg::EndLog();
	
	uint64_t config = frigg::volatileRead<uint64_t>(&hpetRegs[kHpetGenConfig]);
	config |= kHpetEnable;
	frigg::volatileWrite<uint64_t>(&hpetRegs[kHpetGenConfig], config);
	
	infoLogger->log() << "Enabled HPET" << frigg::EndLog();
	
	// disable the legacy PIT (i.e. program to one-shot mode)
	frigg::arch_x86::ioOutByte(kPitCommand, kPitOnTerminalCount | kPitLowHigh);
	frigg::arch_x86::ioOutByte(kPitChannel0, 1);
	frigg::arch_x86::ioOutByte(kPitChannel0, 0);
	
	// program hpet timer 0 in one-shot mode
	uint64_t timer_config = frigg::volatileRead<uint64_t>(&hpetRegs[kHpetTimerConfig0]);
	timer_config &= ~int64_t(0x1F << kHpetIntShift);
	timer_config |= 2 << kHpetIntShift;
	timer_config |= kHpetEnableInt;
	frigg::volatileWrite<uint64_t>(&hpetRegs[kHpetTimerConfig0], timer_config);
	frigg::volatileWrite<uint64_t>(&hpetRegs[kHpetTimerComparator0], 0);

	calibrateApicTimer();

	timerQueue.initialize(*kernelAlloc);
}

void pollSleepNano(uint64_t nanotime) {
	uint64_t counter = frigg::volatileRead<uint64_t>(&hpetRegs[kHpetMainCounter]);
	uint64_t goal = counter + nanotime * kFemtosPerNano / hpetFrequency;
	while(frigg::volatileRead<uint64_t>(&hpetRegs[kHpetMainCounter]) < goal) {
		frigg::pause();
	}
}

uint64_t currentTicks() {
	return frigg::volatileRead<uint64_t>(&hpetRegs[kHpetMainCounter]);
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

void installTimer(Timer &&timer) {
	if(timerQueue->empty()) {
		frigg::volatileWrite<uint64_t>(&hpetRegs[kHpetTimerComparator0], timer.deadline);
	}

	timerQueue->enqueue(frigg::move(timer));
}

void timerInterrupt() {
	auto current = frigg::volatileRead<uint64_t>(&hpetRegs[kHpetMainCounter]);
	while(!timerQueue->empty() && timerQueue->front().deadline < current) {
		Timer timer = timerQueue->dequeue();

		KernelSharedPtr<Thread> thread = timer.thread.grab();
		if(thread) {
			ScheduleGuard schedule_guard(scheduleLock.get());
			enqueueInSchedule(schedule_guard, thread);
			schedule_guard.unlock();
		}
	}

	if(!timerQueue->empty()) {
		frigg::volatileWrite<uint64_t>(&hpetRegs[kHpetTimerComparator0],
				timerQueue->front().deadline);
	}
}

} // namespace thor

