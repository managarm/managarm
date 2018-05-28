
#include "../../generic/types.hpp"
#include "../../generic/timer.hpp"

namespace thor {

bool haveTimer();

void setupHpet(PhysicalAddr address);

void pollSleepNano(uint64_t nanotime);

uint64_t currentTicks();

uint64_t currentNanos();

uint64_t durationToTicks(uint64_t seconds, uint64_t millis = 0,
		uint64_t micros = 0, uint64_t nanos = 0);

} // namespace thor

