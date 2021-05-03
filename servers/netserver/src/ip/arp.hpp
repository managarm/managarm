#pragma once

#include <async/recurring-event.hpp>
#include <netserver/nic.hpp>
#include <map>

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
	};
	async::result<std::optional<nic::MacAddress>> tryResolve(uint32_t addr,
		uint32_t sender);
	void feedArp(nic::MacAddress destination, arch::dma_buffer_view arpData);
	void updateTable(uint32_t proto, nic::MacAddress hardware);
private:
	Entry &getEntry(uint32_t addr);
	std::map<uint32_t, Entry> table_;
};

Neighbours &neigh4();
