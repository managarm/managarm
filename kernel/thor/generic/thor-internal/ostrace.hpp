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

namespace ostrace {

// Set by the ostrace code one in-kernel ostrace is available.
extern std::atomic<bool> available;

struct Context {
	frg::vector<char, KernelAlloc> buffer{*kernelAlloc};
};

extern PerCpu<Context> context;

// Setup the in-kernel ostrace support.
// This is called during ostrace initialization.
// We only put it into the header for friends declarations.
void setup();

// Emit an in-kernel ostrace event.
void emitBuffer(frg::span<char> payload);

using ItemId = uint64_t;

// Term (e.g., name of an event) that is assigned a short numerical ID on the wire protocol.
struct Term {
	friend void setup();

	constexpr Term(const char *name)
	: name_{name} {}

	ItemId id() const {
		// TODO: We cannot assert(id_), otherwise this breaks when !available.
		return id_;
	}

	const char *name() const {
		return name_;
	}

private:
	ItemId id_{static_cast<ItemId>(0)};
	const char *name_;
};

struct Event : Term {
	constexpr Event(const char *name)
	: Term{name} {}
};

struct UintAttribute : Term {
	using Record = managarm::ostrace::UintAttribute<KernelAlloc>;

	constexpr UintAttribute(const char *name)
	: Term{name} { }

	Record operator() (uint64_t v) {
		Record record{*kernelAlloc};
		record.set_id(static_cast<uint64_t>(id()));
		record.set_v(v);
		return record;
	}
};

template<typename... Args>
void emit(const Event &event, Args... args) {
	if (!available.load(std::memory_order_relaxed))
		return;

	managarm::ostrace::EventRecord<KernelAlloc> eventRecord{*kernelAlloc};
	eventRecord.set_id(static_cast<uint64_t>(event.id()));

	managarm::ostrace::EndOfRecord<KernelAlloc> endOfRecord{*kernelAlloc};

	// Determine the sizes of all records of the event.
	size_t size = 0;
	auto determineSize = [&] (auto &msg) {
		auto ts = msg.size_of_tail();
		size += 8 + ts;
	};
	determineSize(eventRecord);
	(determineSize(args), ...);
	determineSize(endOfRecord);

	{
		auto irqLock = frg::guard(&irqMutex());
		auto *ctx = &context.get();

		ctx->buffer.resize(size);

		// Emit all records to the buffer.
		size_t offset = 0;
		auto emitMsg = [&] (auto &msg) {
			auto ts = msg.size_of_tail();
			bool encodeSuccess = bragi::write_head_tail(msg,
					frg::span<char>(ctx->buffer.data() + offset, 8),
					frg::span<char>(ctx->buffer.data() + offset + 8, ts));
			assert(encodeSuccess);
			offset += 8 + ts;
		};
		emitMsg(eventRecord);
		(emitMsg(args), ...);
		emitMsg(endOfRecord);

		emitBuffer({ctx->buffer.data(), size});
	}
}

} // namespace ostrace

extern ostrace::Event ostEvtArmPreemption;
extern ostrace::Event ostEvtArmCpuTimer;

} // namespace thor
