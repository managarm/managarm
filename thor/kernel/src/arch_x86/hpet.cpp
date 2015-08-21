
#include "../kernel.hpp"

namespace thor {

enum {
	kHpetGenCapsAndId = 0,
	kHpetGenConfig = 2,
	kHpetGenIntStatus = 4,
	kHpetMainCounter = 30
};

enum {
	// kHpetCapsAndId register
	kHpet64BitCounter = 0x2000,

	// kHpetGenConfig register
	kHpetEnable = 1
};

enum : uint64_t {
	kFemtosPerNano = 1000000,
	kFemtosPerMicro = kFemtosPerNano * 1000,
	kFemtosPerMilli = kFemtosPerMicro * 1000,
	kFemtosPerSecond = kFemtosPerMilli * 1000
};

uint64_t *hpetRegs;
uint64_t hpetPeriod;

void setupHpet(PhysicalAddr address) {
	infoLogger->log() << "HPET at " << (void *)address << frigg::debug::Finish();
	hpetRegs = accessPhysical<uint64_t>(address);

	uint64_t caps = frigg::atomic::volatileRead<uint64_t>(&hpetRegs[kHpetGenCapsAndId]);
	if((caps & kHpet64BitCounter) == 0)
		infoLogger->log() << "HPET only has a 32-bit counter" << frigg::debug::Finish();

	hpetPeriod = caps >> 32;
	infoLogger->log() << "HPET period: " << hpetPeriod << frigg::debug::Finish();
	
	uint64_t config = frigg::atomic::volatileRead<uint64_t>(&hpetRegs[kHpetGenConfig]);
	frigg::atomic::volatileWrite<uint64_t>(&hpetRegs[kHpetGenConfig],
			config | kHpetEnable);
	
	infoLogger->log() << "Enabled HPET" << frigg::debug::Finish();

	calibrateApicTimer();
}

void pollSleepNano(uint64_t nanotime) {
	uint64_t counter = frigg::atomic::volatileRead<uint64_t>(&hpetRegs[kHpetMainCounter]);
	uint64_t goal = counter + nanotime * kFemtosPerNano / hpetPeriod;
	while(frigg::atomic::volatileRead<uint64_t>(&hpetRegs[kHpetMainCounter]) < goal) {
		frigg::atomic::pause();
	}
}

} // namespace thor

