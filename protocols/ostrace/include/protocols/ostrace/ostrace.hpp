#pragma once

#include <span>
#include <string>

#include <async/queue.hpp>
#include <async/result.hpp>
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
		assert(ctx_);
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

// Lifetime:
//   * The Vocabulary needs to outlive the Context.
struct Context {
	Context(Vocabulary &vocabulary);

	async::result<void> create();

	inline helix::BorrowedLane getLane() {
		return lane_;
	}

	// Whether ostrace is currently active or not.
	inline bool isActive() {
		return enabled_;
	}

	async::result<void> define(Term *term) {
		assert(!term->ctx_);
		auto id = co_await announceItem_(term->name());
		term->ctx_ = this;
		term->id_ = id;
	}

	template<typename... Args>
	void emit(const Event &event, Args... args) {
		if (!isActive())
			return;

		assert(event.ctx() == this);
		([&] (auto *attr) {
			assert(attr->ctx() == this);
		}(args.first), ...);

		managarm::ostrace::EventRecord eventRecord;
		eventRecord.set_id(static_cast<uint64_t>(event.id()));

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

private:
	async::result<ItemId> announceItem_(std::string_view name);
	async::result<void> run_();

	Vocabulary *vocabulary_;
	helix::UniqueLane lane_;
	bool enabled_;
	async::queue<std::vector<char>, frg::stl_allocator> queue_;
};

struct Timer {
	Timer() {
		HEL_CHECK(helGetClock(&_start));
	}

	Timer(const Timer &) = delete;
	Timer &operator= (const Timer &) = delete;

	uint64_t elapsed() {
		uint64_t now;
		HEL_CHECK(helGetClock(&now));
		return now - _start;
	}

private:
	uint64_t _start{0};
};

} // namespace protocols::ostrace
