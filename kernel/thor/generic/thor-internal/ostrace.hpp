#pragma once

#include <atomic>

#include <thor-internal/main.hpp>
#include <thor-internal/ring-buffer.hpp>
#include <ostrace.frigg_bragi.hpp>

namespace thor {

extern bool wantOsTrace;
extern std::atomic<bool> osTraceInUse;

LogRingBuffer *getGlobalOsTraceRing();

initgraph::Stage *getOsTraceAvailableStage();

} // namespace thor
