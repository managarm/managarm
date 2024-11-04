#pragma once

#include <string>

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <ostrace.bragi.hpp>

namespace protocols::ostrace {

enum class EventId : uint64_t {};
enum class ItemId : uint64_t {};

struct Context {
	Context();
	Context(helix::UniqueLane lane, bool enabled);

	inline helix::BorrowedLane getLane() { return lane_; }

	// Whether ostrace is currently active or not.
	inline bool isActive() { return enabled_; }

	async::result<EventId> announceEvent(std::string_view name);
	async::result<ItemId> announceItem(std::string_view name);

  private:
	helix::UniqueLane lane_;
	bool enabled_;
};

struct Event {
	Event(Context *ctx, EventId id);

	void withCounter(ItemId id, int64_t value);

	async::result<void> emit();

  private:
	Context *ctx_;
	bool live_; // Whether we emit an event at all.
	managarm::ostrace::EmitEventReq req_;
};

async::result<Context> createContext();

} // namespace protocols::ostrace
