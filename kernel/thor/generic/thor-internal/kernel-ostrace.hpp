#pragma once

#include <assert.h>
#include <atomic>
#include <bit>
#include <utility>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/functional.hpp>
#include <frg/span.hpp>
#include <frg/utility.hpp>
#include <thor-internal/arch-generic/timer.hpp>
#include <thor-internal/cpu-data.hpp>
#include <ostrace.frigg_bragi.hpp>

namespace thor {

namespace ostrace {

// Set by the ostrace code one in-kernel ostrace is available.
extern std::atomic<bool> available;

// Interval (in milliseconds) at which the metrics sampler reports counters. Zero disables it.
extern uint64_t metricsInterval;

// Setup the in-kernel ostrace support.
// This is called during ostrace initialization.
void setup();

using ItemId = uint64_t;

// Allocator for bragi messages that emit() writes.
// Since it cannot allocate, messages with dynamic fields fail to compile.
struct NullAllocator { };

// Term (e.g., name of an event) that is assigned a short numerical ID on the wire protocol.
// Terms need to be global variables, such that they are constructed before setup() runs.
struct Term {
	// Assigns the ID and serializes the Definition.
	Term(const char *name);

	ItemId id() const {
		return id_;
	}

	const char *name() const {
		return name_;
	}

	// Serialized Definition record of the term.
	frg::span<const char> definition() const {
		return definition_;
	}

private:
	ItemId id_{static_cast<ItemId>(0)};
	const char *name_;
	frg::span<const char> definition_;
};

struct Event : Term {
	Event(const char *name)
	: Term{name} {}
};

struct UintAttribute : Term {
	using Record = managarm::ostrace::UintAttribute<NullAllocator>;

	UintAttribute(const char *name)
	: Term{name} { }

	std::pair<const UintAttribute *, Record> operator() (uint64_t v) const {
		Record record;
		record.set_id(static_cast<uint64_t>(id()));
		record.set_v(v);
		return {this, std::move(record)};
	}
};

// Number of per-CPU counter slots. Each Counter takes one slot, each CounterArray one per index.
// This is a compile time constant such that the counters are available early during initialization.
inline constexpr size_t maxCounterSlots = 1024;
// Upper bound on the size of a CounterArray.
inline constexpr size_t maxCounterArraySize = 64;

struct CounterSlots {
	std::atomic<uint64_t> slots[maxCounterSlots];
};

extern PerCpu<CounterSlots> counterSlots;

// Per-CPU count that the metrics sampler reports periodically as an attribute of thor.metrics.
// Like Term, must be namespace-scope variables.
struct Counter : Term {
	Counter(const char *name);

	// Can be called from any context (including NMIs, and while rescheduling or idling).
	// Also works if ostrace is disabled.
	void add(uint64_t n = 1) const {
		counterSlots.get().slots[slot_].fetch_add(n, std::memory_order_relaxed);
	}

	uint64_t read(size_t cpu) const {
		return counterSlots.getFor(cpu).slots[slot_].load(std::memory_order_relaxed);
	}

	size_t slot() const {
		return slot_;
	}

	const Counter *next() const {
		return next_;
	}

private:
	size_t slot_;
	const Counter *next_;
};

// Array of per-CPU counts that the metrics sampler reports periodically as an event
// with one attribute per non-zero index.
// Same storage requirements as Counter.
struct CounterArray : Term {
	CounterArray(const char *name, size_t size);

	// Can be called from any context, see Counter::add().
	void add(size_t index, uint64_t n = 1) const {
		assert(index < size_);
		counterSlots.get().slots[slot_ + index].fetch_add(n, std::memory_order_relaxed);
	}

	uint64_t read(size_t cpu, size_t index) const {
		assert(index < size_);
		return counterSlots.getFor(cpu).slots[slot_ + index].load(std::memory_order_relaxed);
	}

	size_t slot() const {
		return slot_;
	}

	size_t size() const {
		return size_;
	}

	const CounterArray *next() const {
		return next_;
	}

private:
	size_t slot_;
	size_t size_;
	const CounterArray *next_;
};

// Per-CPU histogram with logarithmic buckets: bucket i counts the values v with std::bit_width(v) == i.
// The last of the size buckets also counts all larger values.
// Same storage requirements as Counter.
struct Histogram : CounterArray {
	Histogram(const char *name, size_t size = maxCounterArraySize)
	: CounterArray{name, size} {
		assert(size);
	}

	// Can be called from any context, see Counter::add().
	void record(uint64_t v) const {
		add(frg::min(static_cast<size_t>(std::bit_width(v)), size() - 1));
	}
};

// Writes an event into the active page of the current CPU.
// Emits the definitions of all terms that the page does not define yet, and then calls write()
// on a span of exactly size bytes.
void writeEvent(frg::span<const Term *const> terms, size_t size,
		frg::function_ref<void(frg::span<char>)> write);

// Size of a record in a page, including its head.
template<typename Msg>
size_t recordSize(Msg &msg) {
	return 8 + msg.size_of_tail();
}

// Writes a record to buffer at offset and advances offset.
template<typename Msg>
void writeRecord(Msg &msg, frg::span<char> buffer, size_t &offset) {
	auto ts = msg.size_of_tail();
	bool encodeSuccess = bragi::write_head_tail(msg,
			frg::span<char>(buffer.data() + offset, 8),
			frg::span<char>(buffer.data() + offset + 8, ts));
	assert(encodeSuccess);
	offset += 8 + ts;
}

// Emit an in-kernel ostrace event. This can be called from any context except for NMIs,
// including while holding scheduler locks.
template<typename... Args>
void emit(const Event &event, Args... args) {
	if (!available.load(std::memory_order_acquire))
		return;

	assert(event.id());
	([&] (const Term *attr) {
		assert(attr->id());
	}(args.first), ...);

	managarm::ostrace::EventRecord<NullAllocator> eventRecord;
	eventRecord.set_id(static_cast<uint64_t>(event.id()));
	eventRecord.set_ts(getClockNanos());

	managarm::ostrace::EndOfRecord<NullAllocator> endOfRecord;

	// Determine the sizes of all records of the event.
	size_t size = recordSize(eventRecord);
	((size += recordSize(args.second)), ...);
	size += recordSize(endOfRecord);

	// Emit all records to the page.
	auto write = [&] (frg::span<char> buffer) {
		size_t offset = 0;
		writeRecord(eventRecord, buffer, offset);
		(writeRecord(args.second, buffer, offset), ...);
		writeRecord(endOfRecord, buffer, offset);
	};

	const Term *terms[] = {&event, args.first...};
	writeEvent({terms, 1 + sizeof...(Args)}, size, write);
}

} // namespace ostrace

} // namespace thor
