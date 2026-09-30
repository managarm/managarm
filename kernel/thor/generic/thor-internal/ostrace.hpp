#pragma once

#include <atomic>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/span.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/ring-buffer.hpp>
#include <ostrace.frigg_bragi.hpp>

namespace thor {

extern bool wantOsTrace;

LogRingBuffer *getGlobalOsTraceRing();

initgraph::Stage *getOsTraceAvailableStage();

void commitFrame(uint64_t source, uint64_t writer, uint64_t firstEvent, uint64_t numEvents,
		frg::span<const char> page);

} // namespace thor
