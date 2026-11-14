#pragma once

#include <stdint.h>

#include <initgraph.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

bool haveHpet();

void setupHpet(PhysicalAddr address);

void pollHpetSleepNano(uint64_t nanotime);

initgraph::Stage *getHpetInitializedStage();

} // namespace thor
