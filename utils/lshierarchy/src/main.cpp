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
	uint64_t chargedBytes;
	std::string tag;

	std::vector<uint64_t> children;
	// Sum of chargedBytes over this node and all of its descendants.
	uint64_t cumulativeBytes = 0;
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
				std::move(node.tag()), {}, 0});

	co_return nodes;
}

std::string formatBytes(uint64_t bytes) {
	if(bytes < 1024)
		return std::format("{} B", bytes);
	const char *units[] = {"KiB", "MiB", "GiB", "TiB"};
	double value = static_cast<double>(bytes) / 1024;
	size_t i = 0;
	while(value >= 1024 && i + 1 < std::size(units)) {
		value /= 1024;
		i++;
	}
	return std::format("{:.1f} {}", value, units[i]);
}

// Accumulates cumulativeBytes bottom-up, sorts children by size and adds this subtree's total to outBytes.
void accumulate(std::map<uint64_t, Node> &byId, uint64_t id, uint64_t &outBytes) {
	auto &node = byId.at(id);
	node.cumulativeBytes = node.chargedBytes;
	for(auto child : node.children)
		accumulate(byId, child, node.cumulativeBytes);
	std::ranges::sort(node.children, [&] (uint64_t x, uint64_t y) {
		return byId.at(x).cumulativeBytes > byId.at(y).cumulativeBytes;
	});
	outBytes += node.cumulativeBytes;
}

void printSubtree(std::map<uint64_t, Node> &byId, uint64_t id, std::string prefix, bool last,
		bool isRoot) {
	auto &node = byId.at(id);

	std::print("{}", prefix);
	if(!isRoot)
		std::print("{}", last ? "`- " : "|- ");
	std::println("{} (#{}): {} charged, {} cumulative",
			node.tag, node.id,
			formatBytes(node.chargedBytes), formatBytes(node.cumulativeBytes));

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
	for(auto id : roots)
		accumulate(byId, id, total);

	std::ranges::sort(roots, [&] (uint64_t x, uint64_t y) {
		return byId.at(x).cumulativeBytes > byId.at(y).cumulativeBytes;
	});

	for(auto id : roots)
		printSubtree(byId, id, "", true, true);

	std::println("");
	std::println("{} nodes, {} charged in total", byId.size(), formatBytes(total));
}

} // anonymous namespace

int main() {
	async::run(run(), helix::currentDispatcher);
}
