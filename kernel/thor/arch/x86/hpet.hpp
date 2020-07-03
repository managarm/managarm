
#include <thor-internal/types.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

bool haveTimer();

void setupHpet(PhysicalAddr address);

void pollSleepNano(uint64_t nanotime);

} // namespace thor

