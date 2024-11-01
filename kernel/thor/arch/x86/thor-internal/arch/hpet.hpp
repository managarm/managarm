#pragma once

#include <stdint.h>

#include <initgraph.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/types.hpp>

namespace thor {

bool haveTimer();

void setupHpet(PhysicalAddr address);

void pollSleepNano(uint64_t nanotime);

initgraph::Stage *getHpetInitializedStage();

} // namespace thor
