
#include "../../generic/types.hpp"
#include "../../generic/timer.hpp"

namespace thor {

bool haveTimer();

void setupHpet(PhysicalAddr address);

void pollSleepNano(uint64_t nanotime);

} // namespace thor

