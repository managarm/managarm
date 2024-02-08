
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/auxv.h>
#include <algorithm>
#include <iostream>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <queue>
#include <variant>
#include <vector>

#include <frg/rbtree.hpp>

#include <async/oneshot-event.hpp>
#include <async/sequenced-event.hpp>
#include <async/result.hpp>
#include <async/queue.hpp>
#include <helix/ipc.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>

#include "mbus.bragi.hpp"

// --------------------------------------------------------
// Entity
// --------------------------------------------------------

struct Entity {
	explicit Entity(int64_t id, uint64_t seq,
			std::unordered_map<std::string, std::string> properties)
	: _id{id}, _seq{seq}, _properties{std::move(properties)} { }

	int64_t id() const {
		return _id;
	}

	uint64_t seq() const {
		return _seq;
	}

	const std::unordered_map<std::string, std::string> &getProperties() const {
		return _properties;
	}

	async::result<void> submitRemoteLane(helix::UniqueLane &&lane) {
		SubmittedLane pending{std::move(lane), {}};
		_submittedLanes.put(&pending);
		co_await pending.complete.wait();
	}

	async::result<helix::UniqueDescriptor> bind();

	frg::rbtree_hook seqNode;
private:
	int64_t _id;
	uint64_t _seq;
	std::unordered_map<std::string, std::string> _properties;

	struct SubmittedLane {
		helix::UniqueLane lane;
		async::oneshot_event complete;
	};

	async::queue<SubmittedLane *, frg::stl_allocator> _submittedLanes;
};

struct EntitySeqLess {
	bool operator()(const Entity &a, const Entity &b) {
		return a.seq() < b.seq();
	}
};

async::result<helix::UniqueDescriptor> Entity::bind() {
	auto pending = *(co_await _submittedLanes.async_get());
	auto lane = std::move(pending->lane);
	pending->complete.raise(); // This destroys *pending

	co_return lane;
}

struct EqualsFilter;
struct Conjunction;

using AnyFilter = std::variant<
	EqualsFilter,
	Conjunction
>;

struct EqualsFilter {
	explicit EqualsFilter(std::string property, std::string value)
	: _property(std::move(property)), _value(std::move(value)) { }

	std::string getProperty() const { return _property; }
	std::string getValue() const { return _value; }

private:
	std::string _property;
	std::string _value;
};

struct Conjunction {
	explicit Conjunction(std::vector<AnyFilter> operands)
	: _operands(std::move(operands)) { }

	const std::vector<AnyFilter> &getOperands() const {
		return _operands;
	}

private:
	std::vector<AnyFilter> _operands;
};

static bool matchesFilter(const Entity *entity, const AnyFilter &filter) {
	if(auto real = std::get_if<EqualsFilter>(&filter); real) {
		auto &properties = entity->getProperties();
		auto it = properties.find(real->getProperty());
		if(it == properties.end())
			return false;
		return it->second == real->getValue();
	}else if(auto real = std::get_if<Conjunction>(&filter); real) {
		auto &operands = real->getOperands();
		return std::all_of(operands.begin(), operands.end(), [&] (const AnyFilter &operand) {
			return matchesFilter(entity, operand);
		});
	}else{
		throw std::runtime_error("Unexpected filter");
	}
}


std::unordered_map<int64_t, std::shared_ptr<Entity>> allEntities;
int64_t nextEntityId = 1;

// Store the entities in an RB tree ordered by their sequence numbers to speed up lookup.
// TODO(qookie): Once we add a way to change properties (which requires a sequence number update),
//               we'll need to protect this tree with an async::mutex if we ever want to make mbus
//               multithreaded (to prevent concurrent update & traversal).
async::sequenced_event globalSeq;
using EntitySeqTree = frg::rbtree<
	Entity,
	&Entity::seqNode,
	EntitySeqLess
>;
EntitySeqTree entitySeqTree;

std::shared_ptr<Entity> getEntityById(int64_t id) {
	auto it = allEntities.find(id);
	if(it == allEntities.end())
		return nullptr;
	return it->second;
}

static AnyFilter decodeFilter(managarm::mbus::AnyFilter &protoFilter) {
	// HACK(qookie): This is a massive hack. I thought bragi had "has_foo" getters, but
	//               apparently I misremembered... We should add them, but for now this
	//               will suffice (and I think we'll get rid of filters on the protocol
	//               level anyway).
	// If the equals filter value is empty, assume this is actually a conjunction.
	if (protoFilter.equals_filter().value().size() == 0) {
		std::vector<AnyFilter> operands;
		for(auto &protoOperand : protoFilter.conjunction().operands()) {
			operands.push_back(EqualsFilter{
						protoOperand.path(),
						protoOperand.value()
					});
		}
		return Conjunction(std::move(operands));
	} else {
		return EqualsFilter{protoFilter.equals_filter().path(),
			protoFilter.equals_filter().value()};
	}
}



async::result<std::tuple<uint64_t, uint64_t>>
tryEnumerate(managarm::mbus::EnumerateResponse &resp, uint64_t inSeq, const AnyFilter &filter) {
	auto actualSeq = co_await globalSeq.async_wait(inSeq);
	auto outSeq = actualSeq;

	// Find the first entity with an interesting seq number.
	auto cur = entitySeqTree.get_root();
	while (cur) {
		if (auto left = EntitySeqTree::get_left(cur);
				left && left->seq() >= inSeq) {
			cur = left;
		} else if (auto right = EntitySeqTree::get_right(cur);
				right && cur->seq() != inSeq) {
			cur = right;
		} else {
			break;
		}
	}

	constexpr size_t maxEntitiesPerMessage = 16;

	// At this point, cur and all successors should have ->seq() >= inSeq
	for (; cur; cur = EntitySeqTree::successor(cur)) {
		assert(cur->seq() >= inSeq);
		// The client doesn't want to see this.
		if (!matchesFilter(cur, filter)) continue;

		managarm::mbus::Entity protoEntity;
		protoEntity.set_id(cur->id());
		for(auto kv : cur->getProperties()) {
			managarm::mbus::Property prop;
			prop.set_name(kv.first);
			prop.set_string_item(kv.second);
			protoEntity.add_properties(prop);
		}

		resp.add_entities(protoEntity);

		// Limit the amount of entities we send at once.
		// Send back the seq number of the successor of the last entity
		// to the client, so it can pick back up where we left off.
		// This is correct since in the non-paginated case, the returned
		// seq number is the seq of the first new entity.
		if (resp.entities().size() >= maxEntitiesPerMessage) {
			outSeq = cur->seq() + 1;
			break;
		}
	}

	co_return {outSeq, actualSeq};
}

async::detached doEnumerate(helix::UniqueLane conversation, uint64_t inSeq, AnyFilter filter) {
	managarm::mbus::EnumerateResponse resp;
	resp.set_error(managarm::mbus::Error::SUCCESS);

	uint64_t curSeq = inSeq;

	while(true) {
		auto [outSeq, actualSeq] = co_await tryEnumerate(resp, curSeq, filter);

		if(!resp.entities().empty()) {
			// At least one entity was added into our response
			resp.set_out_seq(outSeq);
			resp.set_actual_seq(actualSeq);
			break;
		} else {
			// Something changed, but nothing of interest was inserted
			assert(outSeq == actualSeq);
			curSeq = actualSeq;
		}
	}

	auto [sendResp, sendTail] =
		co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
		);
	HEL_CHECK(sendResp.error());
	HEL_CHECK(sendTail.error());
}

async::detached doGetRemoteLane(helix::UniqueLane conversation, std::shared_ptr<Entity> entity) {
	auto remoteLane = co_await entity->bind();

	managarm::mbus::GetRemoteLaneResponse resp;
	resp.set_error(managarm::mbus::Error::SUCCESS);

	auto [sendResp, pushLane] =
		co_await helix_ng::exchangeMsgs(
			conversation,
			helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
			helix_ng::pushDescriptor(remoteLane)
		);
	HEL_CHECK(sendResp.error());
	HEL_CHECK(pushLane.error());
}

async::detached serveMgmtLane(helix::UniqueLane lane, std::shared_ptr<Entity> entity) {
	while(true) {
		auto [accept, recvHead] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		// TODO(qookie): Destroy the entity once the lane is closed.
		HEL_CHECK(accept.error());
		HEL_CHECK(recvHead.error());

		auto conversation = accept.descriptor();

		auto preamble = bragi::read_preamble(recvHead);
		assert(!preamble.error());

		if(preamble.id() == bragi::message_id<managarm::mbus::ServeRemoteLaneRequest>) {
			/* Don't care about the request contents */

			auto [pullLane] =
				co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::pullDescriptor()
				);
			HEL_CHECK(pullLane.error());

			co_await entity->submitRemoteLane(pullLane.descriptor());

			managarm::mbus::ServeRemoteLaneResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

			auto [sendResp] =
				co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
			HEL_CHECK(sendResp.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}

async::detached serve(helix::UniqueLane lane) {
	while(true) {
		auto [accept, recvHead] = co_await helix_ng::exchangeMsgs(
				lane,
				helix_ng::accept(
					helix_ng::recvInline()
				)
			);

		HEL_CHECK(accept.error());
		HEL_CHECK(recvHead.error());

		auto conversation = accept.descriptor();

		auto preamble = bragi::read_preamble(recvHead);
		assert(!preamble.error());

		if(preamble.id() == bragi::message_id<managarm::mbus::GetPropertiesRequest>) {
			auto req = bragi::parse_head_only<managarm::mbus::GetPropertiesRequest>(recvHead);

			managarm::mbus::GetPropertiesResponse resp;
			auto entity = getEntityById(req->id());

			if(!entity) {
				resp.set_error(managarm::mbus::Error::NO_SUCH_ENTITY);
			} else {
				resp.set_error(managarm::mbus::Error::SUCCESS);
				for(auto kv : entity->getProperties()) {
					managarm::mbus::Property prop;
					prop.set_name(kv.first);
					prop.set_string_item(kv.second);
					resp.add_properties(prop);
				}
			}

			auto [sendHead, sendTail] =
				co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadTail(resp, frg::stl_allocator{})
				);
			HEL_CHECK(sendHead.error());
			HEL_CHECK(sendTail.error());
		} else if(preamble.id() == bragi::message_id<managarm::mbus::GetRemoteLaneRequest>) {
			auto req = bragi::parse_head_only<managarm::mbus::GetRemoteLaneRequest>(recvHead);
			auto entity = getEntityById(req->id());
			if(!entity) {
				managarm::mbus::GetRemoteLaneResponse resp;
				resp.set_error(managarm::mbus::Error::NO_SUCH_ENTITY);

				auto [sendResp] =
					co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
					);
				HEL_CHECK(sendResp.error());
				continue;
			}

			doGetRemoteLane(std::move(conversation), std::move(entity));
		} else if(preamble.id() == bragi::message_id<managarm::mbus::EnumerateRequest>) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recvTail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recvTail.error());

			auto req = bragi::parse_head_tail<managarm::mbus::EnumerateRequest>(recvHead, tail);

			doEnumerate(std::move(conversation), req->seq(),
					decodeFilter(req->filter()));
		} else if(preamble.id() == bragi::message_id<managarm::mbus::CreateObjectRequest>) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recvTail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recvTail.error());

			auto req = bragi::parse_head_tail<managarm::mbus::CreateObjectRequest>(recvHead, tail);

			std::unordered_map<std::string, std::string> properties;
			for(auto &kv : req->properties()) {
				properties.insert({ kv.name(), kv.string_item() });
			}

			// TODO(qookie): Introduce async::sequenced_event::current_sequence?
			//               We want the current seq because the input seq from the
			//               user is the seq of the first item to be returned
			//               (e.g. see doEnumerate pagination logic).
			auto seq = globalSeq.next_sequence() - 1;
			auto child = std::make_shared<Entity>(nextEntityId++, seq,
					std::move(properties));

			allEntities.insert({ child->id(), child });
			entitySeqTree.insert(child.get());

			// Wake up all pending enumeration operations.
			globalSeq.raise();

			// Set up the management lane
			auto [localLane, remoteLane] = helix::createStream();
			serveMgmtLane(std::move(localLane), child);

			managarm::mbus::CreateObjectResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);
			resp.set_id(child->id());

			auto [sendResp, pushLane] =
				co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
					helix_ng::pushDescriptor(remoteLane)
				);
			HEL_CHECK(sendResp.error());
			HEL_CHECK(pushLane.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	std::cout << "Entering mbus" << std::endl;

	unsigned long xpipe;
	if(peekauxval(AT_XPIPE, &xpipe))
		throw std::runtime_error("No AT_XPIPE specified");

	serve(helix::UniqueLane(xpipe));
	async::run_forever(helix::currentDispatcher);
}

