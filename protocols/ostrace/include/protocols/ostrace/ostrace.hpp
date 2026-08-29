#pragma once

#include <atomic>
#include <span>
#include <string>

#include <async/mutex.hpp>
#include <async/queue.hpp>
#include <async/result.hpp>
#include <helix/clock.hpp>
#include <helix/ipc.hpp>
#include <ostrace.bragi.hpp>

namespace protocols::ostrace {

enum class ItemId : uint64_t { };

struct Context;

// Term (e.g., name of an event) that is assigned a short numerical ID on the wire protocol.
struct Term {
	friend struct Context;

	constexpr Term(const char *name)
	: name_{name} {}

	Context *ctx() const {
		return ctx_;
	}

	ItemId id() const {
		// TODO: We cannot assert(ctx_), otherwise this breaks when !available.
		return id_;
	}

	const char *name() const {
		return name_;
	}

private:
	Context *ctx_{nullptr};
	ItemId id_{static_cast<ItemId>(0)};
	const char *name_;
};

// Collection of many Terms.
// Lifetime:
//   * All Terms that are passed to the constructor of Vocabulary need to outlive Vocabulary.
//     You typically want to store the Terms with static storage duration.
//   * All Terms that are passed to Vocabulary need to be fully constructed.
//     Use constinit on the Terms to ensure that this holds.
struct Vocabulary {
	template<typename... Terms>
	Vocabulary(Terms &... terms)
	: terms_{{&terms...}} {}

	const auto &terms() { return terms_; }

private:
	std::vector<Term *> terms_;
};

struct Event : Term {
	constexpr Event(const char *name)
	: Term{name} {}
};

struct UintAttribute : Term {
	friend struct Context;

	using Record = managarm::ostrace::UintAttribute;

	constexpr UintAttribute(const char *name)
	: Term{name} { }

	std::pair<UintAttribute *, Record> operator() (uint64_t v) {
		Record record;
		record.set_id(static_cast<uint64_t>(id()));
		record.set_v(v);
		return {this, std::move(record)};
	}
};

struct BragiAttribute : Term {
	friend struct Context;

	using Record = managarm::ostrace::BufferAttribute;

	constexpr BragiAttribute(const char *name)
	: Term{name} { }

	std::pair<BragiAttribute *, Record> operator() (std::span<uint8_t> head, std::span<uint8_t> tail) {
		Record record;
		record.set_id(static_cast<uint64_t>(id()));
		record.set_buffer(std::vector<uint8_t>(head.size_bytes() + tail.size_bytes()));
		std::ranges::copy(head, record.buffer().data());
		if(!tail.empty())
			std::ranges::copy(tail, &record.buffer().at(head.size_bytes()));
		return {this, std::move(record)};
	}
};

// Lifetime:
//   * The Vocabulary needs to outlive the Context.
struct Context {
	Context(Vocabulary &vocabulary);

	// Connects to the ostrace server. Does nothing if the Context is already initialized.
	async::result<void> create();

	// Whether create() already completed. Allows callers on hot paths to skip redundant calls.
	inline bool isInitialized() {
		return initialized_.load(std::memory_order_acquire);
	}

	// Whether ostrace is currently active or not.
	inline bool isActive() {
		return enabled_.load(std::memory_order_acquire);
	}

	async::result<void> define(Term *term) {
		assert(!term->ctx_);
		auto id = co_await announceItem_(term->name());
		term->ctx_ = this;
		term->id_ = id;
	}

	template<typename... Args>
	void emitWithTimestamp(const Event &event, size_t ts, Args... args) {
		if (!isActive())
			return;

		assert(event.ctx() == this);
		([&] (auto *attr) {
			assert(attr->ctx() == this);
		}(args.first), ...);

		managarm::ostrace::EventRecord eventRecord;
		eventRecord.set_id(static_cast<uint64_t>(event.id()));
		eventRecord.set_ts(ts);

		managarm::ostrace::EndOfRecord endOfRecord;

		// Determine the sizes of all records of the event.
		size_t size = 0;
		auto determineSize = [&] (auto &msg) {
			auto ts = msg.size_of_tail();
			size += 8 + ts;
		};
		determineSize(eventRecord);
		(determineSize(args.second), ...);
		determineSize(endOfRecord);

		std::vector<char> buffer;
		buffer.resize(size);

		// Emit all records to the buffer.
		size_t offset = 0;
		auto emitMsg = [&] (auto &msg) {
			auto ts = msg.size_of_tail();
			bool encodeSuccess = bragi::write_head_tail(msg,
					std::span<char>(buffer.data() + offset, 8),
					std::span<char>(buffer.data() + offset + 8, ts));
			assert(encodeSuccess);
			offset += 8 + ts;
		};
		emitMsg(eventRecord);
		(emitMsg(args.second), ...);
		emitMsg(endOfRecord);

		queue_.put(std::move(buffer));
	}

	template<typename... Args>
	void emit(const Event &event, Args... args) {
		if (!isActive())
			return;
		auto ts = helix::getClock();
		emitWithTimestamp(event, ts, std::forward<Args>(args)...);
	}

private:
	async::result<ItemId> announceItem_(std::string_view name);
	async::result<void> run_();

	Vocabulary *vocabulary_;
	async::mutex initMutex_;
	std::atomic<bool> initialized_ = false;
	helix::UniqueLane lane_;
	std::atomic<bool> enabled_ = false;
	async::queue<std::vector<char>, frg::stl_allocator> queue_;
};

struct Timer {
	Timer()
	: _start{helix::getClock()} {
		_split = _start;
	}

	Timer(const Timer &) = delete;
	Timer &operator= (const Timer &) = delete;

	uint64_t elapsed() {
		return helix::getClock() - _start;
	}

	// Ends the current phase of the timed operation and returns its duration.
	// Storing the result in a zero-initialized variable keeps phases that an early return never
	// reaches at zero, whereas subtracting stored timestamps would underflow.
	uint64_t split() {
		auto now = helix::getClock();
		auto duration = now - _split;
		_split = now;
		return duration;
	}

private:
	uint64_t _start{0};
	uint64_t _split{0};
};

} // namespace protocols::ostrace
