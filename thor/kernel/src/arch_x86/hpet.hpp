
namespace thor {

void setupHpet(PhysicalAddr address);

void pollSleepNano(uint64_t nanotime);

struct Timer {
	Timer(uint64_t deadline)
	: deadline(deadline) { }

	bool operator< (const Timer &other) {
		return deadline < other.deadline;
	}

	uint64_t deadline;
};

uint64_t currentTicks();

uint64_t durationToTicks(uint64_t seconds, uint64_t nanos = 0);

void installTimer(Timer &&timer);

void timerInterrupt();

} // namespace thor

