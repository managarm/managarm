
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
#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>

#include "mbus.pb.h"

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

COFIBER_ROUTINE(async::result<helix::UniqueDescriptor>, Object::bind(), ([=] {
	helix::Offer offer;
	helix::SendBuffer send_req;
	helix::RecvBuffer recv_resp;
	helix::PullDescriptor pull_desc;

	managarm::mbus::SvrRequest req;
	req.set_req_type(managarm::mbus::SvrReqType::BIND);

	auto ser = req.SerializeAsString();
	uint8_t buffer[128];
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&offer, kHelItemAncillary),
			helix::action(&send_req, ser.data(), ser.size(), kHelItemChain),
			helix::action(&recv_resp, buffer, 128, kHelItemChain),
			helix::action(&pull_desc));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(offer.error());
	HEL_CHECK(send_req.error());
	HEL_CHECK(recv_resp.error());
	HEL_CHECK(pull_desc.error());

	managarm::mbus::CntResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	COFIBER_RETURN(pull_desc.descriptor());
}))

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

	cofiber::no_future traverse(std::shared_ptr<Entity> root);

	cofiber::no_future onAttach(std::shared_ptr<Entity> entity);

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
		observer_ptr->onAttach(entity);
}

COFIBER_ROUTINE(cofiber::no_future, Observer::traverse(std::shared_ptr<Entity> root), ([=] {
	std::queue<std::shared_ptr<Entity>> entities;
	entities.push(root);
	while(!entities.empty()) {
		std::shared_ptr<Entity> entity = entities.front();
		entities.pop();
		if(typeid(*entity) == typeid(Group)) {
			auto group = std::static_pointer_cast<Group>(entity);
			for(auto child : group->getChildren())
				entities.push(std::move(child));
		}

		if(!matchesFilter(entity.get(), _filter)) 
			continue;
		
		helix::SendBuffer send_req;

		managarm::mbus::SvrRequest req;
		req.set_req_type(managarm::mbus::SvrReqType::ATTACH);
		req.set_id(entity->getId());
		for(auto kv : entity->getProperties()) {
			auto entry = req.add_properties();
			entry->set_name(kv.first);
			entry->mutable_item()->mutable_string_item()->set_value(kv.second);
		}

		auto ser = req.SerializeAsString();
		auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
				helix::action(&send_req, ser.data(), ser.size()));
		COFIBER_AWAIT transmit.async_wait();
		HEL_CHECK(send_req.error());
	}
}))

COFIBER_ROUTINE(cofiber::no_future, Observer::onAttach(std::shared_ptr<Entity> entity), ([=] {
	if(!matchesFilter(entity.get(), _filter)) 
		return;
	
	helix::SendBuffer send_req;

	managarm::mbus::SvrRequest req;
	req.set_req_type(managarm::mbus::SvrReqType::ATTACH);
	req.set_id(entity->getId());
	req.set_id(entity->getId());
	for(auto kv : entity->getProperties()) {
		auto entry = req.add_properties();
		entry->set_name(kv.first);
		entry->mutable_item()->mutable_string_item()->set_value(kv.second);
	}

	auto ser = req.SerializeAsString();
	auto &&transmit = helix::submitAsync(_lane, helix::Dispatcher::global(),
			helix::action(&send_req, ser.data(), ser.size()));
	COFIBER_AWAIT transmit.async_wait();
	HEL_CHECK(send_req.error());
}))

std::unordered_map<int64_t, std::shared_ptr<Entity>> allEntities;
int64_t nextEntityId = 1;

static AnyFilter decodeFilter(const managarm::mbus::AnyFilter &proto_filter) {
	if(proto_filter.type_case() == managarm::mbus::AnyFilter::kEqualsFilter) {
		return EqualsFilter(proto_filter.equals_filter().path(),
				proto_filter.equals_filter().value());
	}else if(proto_filter.type_case() == managarm::mbus::AnyFilter::kConjunction) {
		std::vector<AnyFilter> operands;
		for(auto &proto_operand : proto_filter.conjunction().operands())
			operands.push_back(decodeFilter(proto_operand));
		return Conjunction(std::move(operands));
	}else{
		throw std::runtime_error("Unexpected filter message");
	}
}

COFIBER_ROUTINE(cofiber::no_future, serve(helix::UniqueLane p),
		([lane = std::move(p)] () {
	while(true) {
		helix::Accept accept;
		helix::RecvBuffer recv_req;

		char buffer[256];
		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req, buffer, 256));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::mbus::CntRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.req_type() == managarm::mbus::CntReqType::GET_ROOT) {
			helix::SendBuffer send_resp;

			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);
			resp.set_id(1);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size()));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
		}else if(req.req_type() == managarm::mbus::CntReqType::CREATE_OBJECT) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_lane;

			auto parent = allEntities.at(req.parent_id());
			if(typeid(*parent) != typeid(Group))
				throw std::runtime_error("Objects can only be created inside groups");
			auto group = std::static_pointer_cast<Group>(parent);

			std::unordered_map<std::string, std::string> properties;
			for(auto &kv : req.properties()) {
				assert(kv.has_item() && kv.item().has_string_item());
				properties.insert({ kv.name(), kv.item().string_item().value() });
			}

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto child = std::make_shared<Object>(nextEntityId++,
					group, std::move(properties), std::move(local_lane));
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

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_lane, remote_lane));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_lane.error());
		}else if(req.req_type() == managarm::mbus::CntReqType::LINK_OBSERVER) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_lane;

			auto parent = allEntities.at(req.id());
			if(typeid(*parent) != typeid(Group))
				throw std::runtime_error("Observers can only be attached to groups");
			auto group = std::static_pointer_cast<Group>(parent);

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			auto observer = std::make_shared<Observer>(decodeFilter(req.filter()),
					std::move(local_lane));
			group->linkObserver(observer);

			observer->traverse(parent);

			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_lane, remote_lane));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_lane.error());
		}else if(req.req_type() == managarm::mbus::CntReqType::BIND2) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor send_desc;

			auto entity = allEntities.at(req.id());
			if(typeid(*entity) != typeid(Object))
				throw std::runtime_error("Bind can only be invoked on objects");
			auto object = std::static_pointer_cast<Object>(entity);

			auto descriptor = COFIBER_AWAIT object->bind();
			
			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&send_desc, descriptor));
			COFIBER_AWAIT transmit.async_wait();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_desc.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}))

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

	{
		async::queue_scope scope{helix::globalQueue()};
		serve(helix::UniqueLane(xpipe));
	}

	helix::globalQueue()->run();
}

