#pragma once

#include <thor-internal/initgraph.hpp>
#include <thor-internal/types.hpp>
#include <thor-internal/timer.hpp>

namespace thor {

bool haveTimer();

void setupHpet(PhysicalAddr address);

void pollSleepNano(uint64_t nanotime);

initgraph::Stage *getHpetInitializedStage();

} // namespace thor
