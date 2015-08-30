
namespace thor {

void setupHpet(PhysicalAddr address);

void pollSleepNano(uint64_t nanotime);

uint64_t currentTicks();

uint64_t durationToTicks(uint64_t seconds, uint64_t millis = 0,
		uint64_t micros = 0, uint64_t nanos = 0);

class Timer;

void installTimer(Timer &&timer);

void timerInterrupt();

} // namespace thor

