#include <algorithm>
#include <map>
#include <print>
#include <string>
#include <vector>

#include <bragi/helpers-std.hpp>
#include <helix/ipc.hpp>
#include <kerncfg.bragi.hpp>
#include <protocols/mbus/client.hpp>

namespace {

struct Node {
	uint64_t id;
	uint64_t parentId;
	uint64_t chargedMemory;
	uint64_t chargedSwap;
	std::string tag;

	std::vector<uint64_t> children;
	// Sums of chargedMemory and chargedSwap over this node and all of its descendants.
	uint64_t cumulativeMemory = 0;
	uint64_t cumulativeSwap = 0;
};

async::result<std::vector<Node>> fetchHierarchy() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"class", "kerncfg"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	auto [_, events] = (co_await enumerator.nextEvents()).unwrap();
	assert(events.size() == 1);

	auto entity = co_await mbus_ng::Instance::global().getEntity(events[0].id);
	auto lane = (co_await entity.getRemoteLane()).unwrap();

	managarm::kerncfg::GetHierarchyRequest req;

	auto [offer, sendReq, recvHead] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::offer(
				helix_ng::want_lane,
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvHead.error());

	auto preamble = bragi::read_preamble(recvHead);
	assert(!preamble.error());

	auto resp = *bragi::parse_head_only<managarm::kerncfg::GetHierarchyResponse>(recvHead);
	recvHead.reset();

	std::vector<std::byte> tailBuffer(preamble.tail_size());
	auto [recvTail] = co_await helix_ng::exchangeMsgs(
			offer.descriptor(),
			helix_ng::recvBuffer(tailBuffer.data(), tailBuffer.size())
		);

	HEL_CHECK(recvTail.error());

	bragi::limited_reader reader{tailBuffer.data(), tailBuffer.size()};
	auto ok = resp.decode_tail(reader);
	assert(ok);
	assert(resp.error() == managarm::kerncfg::Error::SUCCESS);

	std::vector<Node> nodes;
	nodes.reserve(resp.nodes_size());
	for(auto &node : resp.nodes())
		nodes.push_back({node.id(), node.parent_id(), node.charged_memory(),
				node.charged_swap(), std::move(node.tag()), {}, 0, 0});

	co_return nodes;
}

std::string formatBytes(uint64_t bytes) {
	if(!bytes)
		return "-";
	if(bytes < 1024)
		return std::format("{}B", bytes);
	const char *units[] = {"K", "M", "G", "T"};
	double value = static_cast<double>(bytes) / 1024;
	size_t i = 0;
	while(value >= 1024 && i + 1 < std::size(units)) {
		value /= 1024;
		i++;
	}
	return std::format("{:.1f}{}", value, units[i]);
}

// Accumulates cumulativeMemory and cumulativeSwap bottom-up, sorts children by size
// and adds this subtree's totals to outBytes and outSwapBytes.
void accumulate(std::map<uint64_t, Node> &byId, uint64_t id, uint64_t &outBytes,
		uint64_t &outSwapBytes) {
	auto &node = byId.at(id);
	node.cumulativeMemory = node.chargedMemory;
	node.cumulativeSwap = node.chargedSwap;
	for(auto child : node.children)
		accumulate(byId, child, node.cumulativeMemory, node.cumulativeSwap);
	std::ranges::sort(node.children, [&] (uint64_t x, uint64_t y) {
		return byId.at(x).cumulativeMemory > byId.at(y).cumulativeMemory;
	});
	outBytes += node.cumulativeMemory;
	outSwapBytes += node.cumulativeSwap;
}

void printSubtree(std::map<uint64_t, Node> &byId, uint64_t id, std::string prefix, bool last,
		bool isRoot) {
	auto &node = byId.at(id);

	std::println("{:>7} {:>7} {:>7} {:>7}  {}{}{} #{}",
			formatBytes(node.chargedMemory), formatBytes(node.cumulativeMemory),
			formatBytes(node.chargedSwap), formatBytes(node.cumulativeSwap),
			prefix, isRoot ? "" : (last ? "`- " : "|- "), node.tag, node.id);

	auto childPrefix = prefix;
	if(!isRoot)
		childPrefix += last ? "   " : "|  ";

	for(size_t i = 0; i < node.children.size(); i++)
		printSubtree(byId, node.children[i], childPrefix,
				i + 1 == node.children.size(), false);
}

async::result<void> run() {
	auto nodes = co_await fetchHierarchy();

	std::map<uint64_t, Node> byId;
	for(auto &node : nodes)
		byId.insert({node.id, node});

	std::vector<uint64_t> roots;
	for(auto &[id, node] : byId) {
		// Only the root hierarchy has no parent: live children keep their parents alive.
		if(node.parentId && byId.contains(node.parentId)) {
			byId.at(node.parentId).children.push_back(id);
		}else{
			roots.push_back(id);
		}
	}

	uint64_t total = 0;
	uint64_t totalSwap = 0;
	for(auto id : roots)
		accumulate(byId, id, total, totalSwap);

	std::ranges::sort(roots, [&] (uint64_t x, uint64_t y) {
		return byId.at(x).cumulativeMemory > byId.at(y).cumulativeMemory;
	});

	std::println("{:>7} {:>7} {:>7} {:>7}  {}", "MEM", "MEM-C", "SWAP", "SWAP-C", "HIERARCHY");
	for(auto id : roots)
		printSubtree(byId, id, "", true, true);

	std::println("");
	std::println("{} nodes, {} memory, {} swap", byId.size(), formatBytes(total),
			formatBytes(totalSwap));
}

} // anonymous namespace

int main() {
	async::run(run(), helix::currentDispatcher);
}
