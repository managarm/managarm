#pragma once

#include <atomic>

#include <ostrace.frigg_bragi.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/ring-buffer.hpp>

namespace thor {

extern bool wantOsTrace;
extern std::atomic<bool> osTraceInUse;

enum class OsTraceEventId : uint64_t {};

LogRingBuffer *getGlobalOsTraceRing();

OsTraceEventId announceOsTraceEvent(frg::string_view name);
void emitOsTrace(managarm::ostrace::EventRecord<KernelAlloc> record);

initgraph::Stage *getOsTraceAvailableStage();

struct OsTraceEvent {
	OsTraceEvent(OsTraceEventId id) {
		live_ = osTraceInUse.load(std::memory_order_relaxed);
		if (live_)
			rec_.set_id(static_cast<uint64_t>(id));
	}

	void emit() {
		if (!live_)
			return;
		emitOsTrace(std::move(rec_));
	}

  private:
	bool live_; // Whether we emit an event at all.
	managarm::ostrace::EventRecord<KernelAlloc> rec_{*kernelAlloc};
};

} // namespace thor
