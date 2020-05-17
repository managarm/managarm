#include "arp.hpp"

#include <helix/ipc.hpp>
#include <helix/timer.hpp>
#include <arch/bit.hpp>
#include <iomanip>
#include "ip4.hpp"

struct ArpHeader {
	uint16_t hrd;
	uint16_t pro;
	uint8_t hln;
	uint8_t pln;
	uint16_t op;
};
static_assert(sizeof(ArpHeader) == 8, "ARP leader struct must be 8 bytes");

namespace {
async::result<void> sendArp(uint16_t op,
		uint32_t sender,
		nic::MacAddress targetHw, uint32_t targetProto) {
	auto ensureEndian = [] (auto &x) {
		using namespace arch;
		x = convert_endian<endian::big, endian::native>(x);
	};
	ArpHeader leader {
		1, static_cast<uint16_t>(nic::ETHER_TYPE_IP4),
		6, 4,
		op
	};

	auto link = ip4().getLink(sender);
	if (!link) {
		co_return;
	}

	auto targetMac = targetHw;
	if (std::all_of(begin(targetMac), end(targetMac),
			[] (auto x) { return x == 0; })) {
		// create broadcast
		std::fill(begin(targetMac), end(targetMac), 0xff);
	}

	ensureEndian(leader.op);
	ensureEndian(leader.hrd);
	ensureEndian(leader.pro);
	ensureEndian(sender);
	ensureEndian(targetProto);

	auto buffer = link->allocateFrame(targetMac, nic::ETHER_TYPE_ARP,
		sizeof(leader)
		+ 2 * sizeof(nic::MacAddress)
		+ 2 * sizeof(uint32_t));
	arch::dma_buffer_view bufv { buffer.payload };
	auto appendData = [&bufv] (auto data) {
		std::memcpy(bufv.data(), &data, sizeof(data));
		bufv = bufv.subview(sizeof(data));
	};

	appendData(leader);

	appendData(link->deviceMac());
	appendData(sender);

	appendData(targetHw);
	appendData(targetProto);
	co_await link->send(std::move(buffer.frame));
}
}

void Neighbours::feedArp(nic::MacAddress dst, arch::dma_buffer_view view) {
	using namespace nic;
	auto ensureEndian = [] (auto &x) {
		using namespace arch;
		x = convert_endian<endian::big, endian::native>(x);
	};
	ArpHeader leader;

	if (view.size() < sizeof(leader)) {
		return;
	}

	std::memcpy(&leader, view.data(), sizeof(leader));
	view = view.subview(sizeof(leader));
	// rest of data: hw sender, proto sender, hw target, proto target
	ensureEndian(leader.hrd);
	ensureEndian(leader.pro);
	ensureEndian(leader.op);

	if (leader.hrd != 1 || leader.pro != ETHER_TYPE_IP4) {
		// ignore non Ethernet, non IP traffic (hrd 1 is eth)
		// I don't believe any other protocols uses arp, since the same
		// address space also covers other MAC protocols such as wifi,
		// and since NDP exists
		return;
	}

	if (leader.hln != 6 || leader.pln != 4) {
		// broken arp? ignore
		return;
	}

	if (view.size() < 2 * leader.hln + 2 * leader.pln) {
		return;
	}

	MacAddress senderHw;
	uint32_t senderProto;
	MacAddress targetHw;
	uint32_t targetProto;

	std::memcpy(senderHw.data(), view.data(), sizeof(senderHw));
	view = view.subview(leader.hln);
	std::memcpy(&senderProto, view.data(), sizeof(senderProto));
	view = view.subview(leader.pln);

	std::memcpy(targetHw.data(), view.data(), sizeof(targetHw));
	view = view.subview(leader.hln);
	std::memcpy(&targetProto, view.data(), sizeof(targetProto));

	ensureEndian(senderProto);
	ensureEndian(targetProto);

	updateTable(senderProto, senderHw);

	if (leader.op != 1) {
		return;
	}

	async::detach(sendArp(2, targetProto, senderHw, senderProto));
}

Neighbours::Entry &Neighbours::getEntry(uint32_t ip) {
	uint64_t time;
	HEL_CHECK(helGetClock(&time));
	if (auto f = table_.find(ip); f != table_.end()) {
		if (time + staleTimeMs * 1'000'000 <= f->second.mtime_ns) {
			f->second.state = State::stale;
		}
		return f->second;
	}
	auto &entry = table_.emplace(std::piecewise_construct,
		std::make_tuple(ip), std::make_tuple()).first->second;
	entry.mtime_ns = time;
	return entry;
}

void Neighbours::updateTable(uint32_t ip, nic::MacAddress mac) {
	auto &entry = getEntry(ip);
	entry.mac = mac;
	entry.state = State::reachable;
	entry.change.ring();
}

namespace {
async::detached entryProber(uint32_t ip, Neighbours::Entry &e, uint32_t sender) {
	e.state = Neighbours::State::probe;
	for (int i = 0; i < 3; i++) {
		co_await sendArp(1, sender, {}, ip);
		std::cout << "netserver: sent arp req" << std::endl;

		async::cancellation_event ev;
		helix::TimeoutCancellation timer { 1'000'000'000, ev };
		co_await e.change.async_wait(ev);
		co_await timer.retire();

		if (e.state != Neighbours::State::probe) {
			co_return;
		}
	}
	e.state = Neighbours::State::failed;
	e.change.ring();
}
} // namespace

async::result<std::optional<nic::MacAddress>> Neighbours::tryResolve(uint32_t ip,
		uint32_t sender) {
	auto &entry = getEntry(ip);
	if (entry.state == State::reachable) {
		co_return entry.mac;
	}
	if (entry.state != State::probe) {
		entryProber(ip, entry, sender);
	}
	co_await entry.change.async_wait();
	if (entry.state != State::reachable) {
		co_return std::nullopt;
	}
	co_return entry.mac;
}

Neighbours &neigh4() {
	static Neighbours neigh;
	return neigh;
}
