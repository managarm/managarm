
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

#include <async/result.hpp>
#include <helix/ipc.hpp>

#include <bragi/helpers-all.hpp>
#include <bragi/helpers-std.hpp>

#include "mbus.bragi.hpp"

// --------------------------------------------------------
// Entity
// --------------------------------------------------------

struct Entity {
	explicit Entity(int64_t id,
			std::unordered_map<std::string, std::string> properties,
			helix::UniqueLane lane)
	: _id(id), _properties(std::move(properties)), _lane(std::move(lane)) { }

	int64_t getId() const {
		return _id;
	}

	const std::unordered_map<std::string, std::string> &getProperties() const {
		return _properties;
	}

	async::result<helix::UniqueDescriptor> bind();

private:
	int64_t _id;
	std::unordered_map<std::string, std::string> _properties;
	helix::UniqueLane _lane;
};

async::result<helix::UniqueDescriptor> Entity::bind() {
	managarm::mbus::S2CBindRequest req;

	auto [offer, sendReq, recvResp, pullLane] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadOnly(req, frg::stl_allocator{}),
				helix_ng::recvInline(),
				helix_ng::pullDescriptor()
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendReq.error());
	HEL_CHECK(recvResp.error());
	HEL_CHECK(pullLane.error());

	auto resp = *bragi::parse_head_only<managarm::mbus::CntResponse>(recvResp);
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	co_return pullLane.descriptor();
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

struct Observer {
	explicit Observer(AnyFilter filter, helix::UniqueLane lane)
	: _filter(std::move(filter)), _lane(std::move(lane)) { }

	async::result<void> onAttach(std::shared_ptr<Entity> entity);

private:
	AnyFilter _filter;
	helix::UniqueLane _lane;
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

async::result<void> Observer::onAttach(std::shared_ptr<Entity> entity) {
	if(!matchesFilter(entity.get(), _filter))
		co_return;

	managarm::mbus::AttachRequest req;
	req.set_id(entity->getId());
	for(auto kv : entity->getProperties()) {
		managarm::mbus::Property prop;
		prop.set_name(kv.first);
		prop.set_string_item(kv.second);
		req.add_properties(prop);
	}

	auto [offer, sendHead, sendTail] =
		co_await helix_ng::exchangeMsgs(
			_lane,
			helix_ng::offer(
				helix_ng::sendBragiHeadTail(req, frg::stl_allocator{})
			)
		);

	HEL_CHECK(offer.error());
	HEL_CHECK(sendHead.error());
	HEL_CHECK(sendTail.error());
}

std::unordered_map<int64_t, std::shared_ptr<Entity>> allEntities;
// NOTE(qookie): ID 1 is for temporarily reserved as the root group ID for backwards compat.
int64_t nextEntityId = 2;

std::vector<std::shared_ptr<Observer>> allObservers;

void processAttach(std::shared_ptr<Entity> entity) {
	for(auto &observer : allObservers)
		async::detach(observer->onAttach(entity));
}

async::detached processObserver(std::shared_ptr<Observer> observer) {
	for(auto &[_, entity] : allEntities)
		co_await observer->onAttach(entity);
}


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
		recvHead.reset();

		if(preamble.id() == bragi::message_id<managarm::mbus::GetRootRequest>) {
			managarm::mbus::SvrResponse resp;
			resp.set_id(1);

			auto [sendResp] =
				co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
				);
			HEL_CHECK(sendResp.error());
		} else if(preamble.id() == bragi::message_id<managarm::mbus::GetPropertiesRequest>) {
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
		} else if(preamble.id() == bragi::message_id<managarm::mbus::CreateObjectRequest>) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recvTail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recvTail.error());

			auto req = bragi::parse_head_tail<managarm::mbus::CreateObjectRequest>(recvHead, tail);

			// NOTE(qookie): ID 1 is the old root group ID.
			if(req->parent_id() != 1) {
				managarm::mbus::SvrResponse resp;
				resp.set_error(managarm::mbus::Error::NO_SUCH_ENTITY);

				auto [sendResp] =
					co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
					);
				HEL_CHECK(sendResp.error());
				continue;
			}

			std::unordered_map<std::string, std::string> properties;
			for(auto &kv : req->properties()) {
				properties.insert({ kv.name(), kv.string_item() });
			}

			helix::UniqueLane localLane, remoteLane;
			std::tie(localLane, remoteLane) = helix::createStream();
			auto child = std::make_shared<Entity>(nextEntityId++,
					std::move(properties), std::move(localLane));
			allEntities.insert({ child->getId(), child });

			// Issue 'attach' events for all observers
			processAttach(child);

			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);
			resp.set_id(child->getId());

			auto [sendResp, pushLane] =
				co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
					helix_ng::pushDescriptor(remoteLane)
				);
			HEL_CHECK(sendResp.error());
			HEL_CHECK(pushLane.error());
		} else if(preamble.id() == bragi::message_id<managarm::mbus::LinkObserverRequest>) {
			std::vector<std::byte> tail(preamble.tail_size());
			auto [recvTail] = co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::recvBuffer(tail.data(), tail.size())
				);
			HEL_CHECK(recvTail.error());

			auto req = bragi::parse_head_tail<managarm::mbus::LinkObserverRequest>(recvHead, tail);

			// NOTE(qookie): ID 1 is the old root group ID.
			if(req->id() != 1) {
				managarm::mbus::SvrResponse resp;
				resp.set_error(managarm::mbus::Error::NO_SUCH_ENTITY);

				auto [sendResp] =
					co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
					);
				HEL_CHECK(sendResp.error());
				continue;
			}

			helix::UniqueLane localLane, remoteLane;
			std::tie(localLane, remoteLane) = helix::createStream();
			auto observer = std::make_shared<Observer>(decodeFilter(req->filter()),
					std::move(localLane));
			allObservers.push_back(observer);

			processObserver(observer);

			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

			auto [sendResp, pushLane] =
				co_await helix_ng::exchangeMsgs(
					conversation,
					helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{}),
					helix_ng::pushDescriptor(remoteLane)
				);
			HEL_CHECK(sendResp.error());
			HEL_CHECK(pushLane.error());
		} else if(preamble.id() == bragi::message_id<managarm::mbus::C2SBindRequest>) {
			auto req = bragi::parse_head_only<managarm::mbus::C2SBindRequest>(recvHead);
			auto entity = getEntityById(req->id());
			if(!entity) {
				managarm::mbus::SvrResponse resp;
				resp.set_error(managarm::mbus::Error::NO_SUCH_ENTITY);

				auto [sendResp] =
					co_await helix_ng::exchangeMsgs(
						conversation,
						helix_ng::sendBragiHeadOnly(resp, frg::stl_allocator{})
					);
				HEL_CHECK(sendResp.error());
				continue;
			}

			auto remoteLane = co_await entity->bind();

			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

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

