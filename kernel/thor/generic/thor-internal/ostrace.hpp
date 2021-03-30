#pragma once

#include <thor-internal/ring-buffer.hpp>

namespace thor {

extern bool wantOsTrace;

LogRingBuffer *getGlobalOsTraceRing();

} // namespace thor
