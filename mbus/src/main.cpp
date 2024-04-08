
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

struct Group;
struct Observer;

struct Entity {
	explicit Entity(int64_t id, std::weak_ptr<Group> parent,
			std::unordered_map<std::string, std::string> properties)
	: _id(id), _parent(std::move(parent)), _properties(std::move(properties)) { }

	virtual ~Entity() { }

	int64_t getId() const {
		return _id;
	}

	std::shared_ptr<Group> getParent() const {
		return _parent.lock();
	}

	const std::unordered_map<std::string, std::string> &getProperties() const {
		return _properties;
	}

private:
	int64_t _id;
	std::weak_ptr<Group> _parent;
	std::unordered_map<std::string, std::string> _properties;
};

struct Group final : Entity {
	explicit Group(int64_t id, std::weak_ptr<Group> parent,
			std::unordered_map<std::string, std::string> properties)
	: Entity(id, std::move(parent), std::move(properties)) { }

	void addChild(std::shared_ptr<Entity> child) {
		_children.insert(std::move(child));
	}

	const std::unordered_set<std::shared_ptr<Entity>> &getChildren() {
		return _children;
	}

	void linkObserver(std::shared_ptr<Observer> observer) {
		_observers.insert(std::move(observer));
	}

	void processAttach(std::shared_ptr<Entity> entity);

private:
	std::unordered_set<std::shared_ptr<Entity>> _children;
	std::unordered_set<std::shared_ptr<Observer>> _observers;
};

struct Object final : Entity {
	explicit Object(int64_t id, std::weak_ptr<Group> parent,
			std::unordered_map<std::string, std::string> properties,
			helix::UniqueLane lane)
	: Entity(id, std::move(parent), std::move(properties)),
			_lane(std::move(lane)) { }

	async::result<helix::UniqueDescriptor> bind();

private:
	helix::UniqueLane _lane;
};

async::result<helix::UniqueDescriptor> Object::bind() {
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

	async::detached traverse(std::shared_ptr<Entity> root);

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

void Group::processAttach(std::shared_ptr<Entity> entity) {
	for(auto &observer_ptr : _observers)
		async::detach(observer_ptr->onAttach(entity));
}

async::detached Observer::traverse(std::shared_ptr<Entity> root) {
	std::queue<std::shared_ptr<Entity>> entities;
	entities.push(root);
	while(!entities.empty()) {
		std::shared_ptr<Entity> entity = entities.front();
		entities.pop();
		if(const Entity &er = *entity; typeid(er) == typeid(Group)) {
			auto group = std::static_pointer_cast<Group>(entity);
			for(auto child : group->getChildren())
				entities.push(std::move(child));
		}

		co_await onAttach(entity);
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
int64_t nextEntityId = 1;

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
			auto parent = getEntityById(req->parent_id());
			if(!parent) {
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

			if(const Entity &pr = *parent; typeid(pr) != typeid(Group))
				throw std::runtime_error("Objects can only be created inside groups");
			auto group = std::static_pointer_cast<Group>(parent);

			std::unordered_map<std::string, std::string> properties;
			for(auto &kv : req->properties()) {
				properties.insert({ kv.name(), kv.string_item() });
			}

			helix::UniqueLane localLane, remoteLane;
			std::tie(localLane, remoteLane) = helix::createStream();
			auto child = std::make_shared<Object>(nextEntityId++,
					group, std::move(properties), std::move(localLane));
			allEntities.insert({ child->getId(), child });

			group->addChild(child);

			// issue 'attach' events for all observers linked to parents of the entity.
			std::shared_ptr<Group> current = group;
			while(current) {
				current->processAttach(child);
				current = current->getParent();
			}

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
			auto parent = getEntityById(req->id());
			if(!parent) {
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

			if(const Entity &pr = *parent; typeid(pr) != typeid(Group))
				throw std::runtime_error("Observers can only be attached to groups");
			auto group = std::static_pointer_cast<Group>(parent);

			helix::UniqueLane localLane, remoteLane;
			std::tie(localLane, remoteLane) = helix::createStream();
			auto observer = std::make_shared<Observer>(decodeFilter(req->filter()),
					std::move(localLane));
			group->linkObserver(observer);

			observer->traverse(parent);

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

			if(const Entity &er = *entity; typeid(er) != typeid(Object))
				throw std::runtime_error("Bind can only be invoked on objects");
			auto object = std::static_pointer_cast<Object>(entity);

			auto remoteLane = co_await object->bind();

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

	auto root = std::make_shared<Group>(nextEntityId++, std::weak_ptr<Group>(),
			std::unordered_map<std::string, std::string>());
	allEntities.insert({ root->getId(), root });

	unsigned long xpipe;
	if(peekauxval(AT_XPIPE, &xpipe))
		throw std::runtime_error("No AT_XPIPE specified");

	serve(helix::UniqueLane(xpipe));
	async::run_forever(helix::currentDispatcher);
}

