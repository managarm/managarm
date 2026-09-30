#pragma once

#include <assert.h>
#include <atomic>
#include <utility>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/functional.hpp>
#include <frg/span.hpp>
#include <thor-internal/arch-generic/timer.hpp>
#include <ostrace.frigg_bragi.hpp>

namespace thor {

namespace ostrace {

// Set by the ostrace code one in-kernel ostrace is available.
extern std::atomic<bool> available;

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

// Writes an event into the active page of the current CPU.
// Emits the definitions of all terms that the page does not define yet, and then calls write()
// on a span of exactly size bytes.
void writeEvent(frg::span<const Term *const> terms, size_t size,
		frg::function_ref<void(frg::span<char>)> write);

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
	size_t size = 0;
	auto determineSize = [&] (auto &msg) {
		auto ts = msg.size_of_tail();
		size += 8 + ts;
	};
	determineSize(eventRecord);
	(determineSize(args.second), ...);
	determineSize(endOfRecord);

	// Emit all records to the page.
	auto write = [&] (frg::span<char> buffer) {
		size_t offset = 0;
		auto emitMsg = [&] (auto &msg) {
			auto ts = msg.size_of_tail();
			bool encodeSuccess = bragi::write_head_tail(msg,
					frg::span<char>(buffer.data() + offset, 8),
					frg::span<char>(buffer.data() + offset + 8, ts));
			assert(encodeSuccess);
			offset += 8 + ts;
		};
		emitMsg(eventRecord);
		(emitMsg(args.second), ...);
		emitMsg(endOfRecord);
	};

	const Term *terms[] = {&event, args.first...};
	writeEvent({terms, 1 + sizeof...(Args)}, size, write);
}

} // namespace ostrace

} // namespace thor
