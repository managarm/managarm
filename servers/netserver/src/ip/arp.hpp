#pragma once

#include <async/recurring-event.hpp>
#include <memory>
#include <netserver/nic.hpp>
#include <map>
#include <optional>

struct Neighbours {
	static constexpr uint64_t staleTimeMs = 30'000;
	enum class State {
		none,
		probe,
		failed,
		reachable,
		stale
	};
	struct Entry {
		uint64_t mtime_ns;
		nic::MacAddress mac;
		async::recurring_event change;
		State state = State::none;
		std::weak_ptr<nic::Link> link;
	};
	async::result<std::optional<nic::MacAddress>> tryResolve(uint32_t addr,
		uint32_t sender);
	void feedArp(nic::MacAddress destination, nic_core::buffer_view arpData, std::weak_ptr<nic::Link> link);
	void updateTable(uint32_t proto, nic::MacAddress hardware, std::weak_ptr<nic::Link> link);
	std::map<uint32_t, Neighbours::Entry> &getTable();
private:
	Entry &getEntry(uint32_t addr);
	std::map<uint32_t, Entry> table_;
};

Neighbours &neigh4();
