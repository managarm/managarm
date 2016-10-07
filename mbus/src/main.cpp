
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
#include <vector>

#include <boost/variant.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>
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
			helix::UniquePipe lane)
	: Entity(id, std::move(parent), std::move(properties)),
			_lane(std::move(lane)) { }

	cofiber::future<helix::UniqueDescriptor> bind();

private:
	helix::UniquePipe _lane;
};

COFIBER_ROUTINE(cofiber::future<helix::UniqueDescriptor>, Object::bind(), ([=] {
	using M = helix::AwaitMechanism;
	
	managarm::mbus::SvrRequest req;
	req.set_req_type(managarm::mbus::SvrReqType::BIND);

	auto serialized = req.SerializeAsString();
	helix::SendString<M> send_req(helix::Dispatcher::global(), _lane,
			serialized.data(), serialized.size(), 0, 0, kHelRequest);
	COFIBER_AWAIT send_req.future();
	HEL_CHECK(send_req.error());

	// recevie and parse the response.
	uint8_t buffer[128];
	helix::RecvString<M> recv_resp(helix::Dispatcher::global(), _lane,
			buffer, 128, 0, 0, kHelResponse);
	COFIBER_AWAIT recv_resp.future();
	HEL_CHECK(recv_resp.error());

	managarm::mbus::CntResponse resp;
	resp.ParseFromArray(buffer, recv_resp.actualLength());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	helix::RecvDescriptor<M> recv_desc(helix::Dispatcher::global(), _lane,
			0, 0, kHelResponse);
	COFIBER_AWAIT recv_desc.future();
	HEL_CHECK(recv_desc.error());
	
	COFIBER_RETURN(recv_desc.descriptor());
}))

struct EqualsFilter;
struct Conjunction;

using AnyFilter = boost::variant<
	EqualsFilter,
	boost::recursive_wrapper<Conjunction>
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
	explicit Observer(AnyFilter filter, helix::UniquePipe lane)
	: _filter(std::move(filter)), _lane(std::move(lane)) { }

	cofiber::no_future traverse(std::shared_ptr<Entity> root);

	cofiber::no_future onAttach(std::shared_ptr<Entity> entity);

private:
	AnyFilter _filter;
	helix::UniquePipe _lane;
};

static bool matchesFilter(const Entity *entity, const AnyFilter &filter) {
	if(filter.type() == typeid(EqualsFilter)) {
		auto &real = boost::get<EqualsFilter>(filter);

		auto &properties = entity->getProperties();
		auto it = properties.find(real.getProperty());
		if(it == properties.end())
			return false;
		return it->second == real.getValue();
	}else if(filter.type() == typeid(Conjunction)) {
		auto &real = boost::get<Conjunction>(filter);

		auto &operands = real.getOperands();
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
	using M = helix::AwaitMechanism;
	
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

		managarm::mbus::SvrRequest req;
		req.set_req_type(managarm::mbus::SvrReqType::ATTACH);
		req.set_id(entity->getId());

		auto serialized = req.SerializeAsString();
		helix::SendString<M> send_req(helix::Dispatcher::global(), _lane,
				serialized.data(), serialized.size(), 0, 0, kHelRequest);
		COFIBER_AWAIT send_req.future();
		HEL_CHECK(send_req.error());
	}
}))

COFIBER_ROUTINE(cofiber::no_future, Observer::onAttach(std::shared_ptr<Entity> entity), ([=] {
	using M = helix::AwaitMechanism;
	
	if(!matchesFilter(entity.get(), _filter)) 
		return;

	managarm::mbus::SvrRequest req;
	req.set_req_type(managarm::mbus::SvrReqType::ATTACH);
	req.set_id(entity->getId());

	auto serialized = req.SerializeAsString();
	helix::SendString<M> send_req(helix::Dispatcher::global(), _lane,
			serialized.data(), serialized.size(), 0, 0, kHelRequest);
	COFIBER_AWAIT send_req.future();
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

COFIBER_ROUTINE(cofiber::no_future, serve(helix::UniquePipe p),
		([lane = std::move(p)] () {
	using M = helix::AwaitMechanism;

	while(true) {
		helix::Accept<M> accept;
		helix::RecvString<M> recv_req;

		helix::submitAsync(lane, {
			helix::action(&accept)
		}, helix::Dispatcher::global());
		COFIBER_AWAIT accept.future();
		HEL_CHECK(accept.error());
		
		auto conversation = accept.descriptor();

		char buffer[256];
		helix::submitAsync(conversation, {
			helix::action(&recv_req, buffer, 256)
		}, helix::Dispatcher::global());
		COFIBER_AWAIT recv_req.future();
		HEL_CHECK(recv_req.error());

		managarm::mbus::CntRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.req_type() == managarm::mbus::CntReqType::GET_ROOT) {
			helix::SendString<M> send_resp;

			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);
			resp.set_id(1);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size())
			}, helix::Dispatcher::global());
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
		}else if(req.req_type() == managarm::mbus::CntReqType::CREATE_OBJECT) {
			helix::SendString<M> send_resp;
			helix::SendDescriptor<M> send_lane;

			auto parent = allEntities.at(req.parent_id());
			if(typeid(*parent) != typeid(Group))
				throw std::runtime_error("Objects can only be created inside groups");
			auto group = std::static_pointer_cast<Group>(parent);

			std::unordered_map<std::string, std::string> properties;
			for(auto &kv : req.properties())
				properties.insert({ kv.first, kv.second });

			helix::UniquePipe local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createFullPipe();
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
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&send_lane, remote_lane)
			}, helix::Dispatcher::global());
			COFIBER_AWAIT send_resp.future();
			COFIBER_AWAIT send_lane.future();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_lane.error());
		}else if(req.req_type() == managarm::mbus::CntReqType::LINK_OBSERVER) {
			helix::SendString<M> send_resp;
			helix::SendDescriptor<M> send_lane;

			auto parent = allEntities.at(req.id());
			if(typeid(*parent) != typeid(Group))
				throw std::runtime_error("Observers can only be attached to groups");
			auto group = std::static_pointer_cast<Group>(parent);

			helix::UniquePipe local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createFullPipe();
			auto observer = std::make_shared<Observer>(decodeFilter(req.filter()),
					std::move(local_lane));
			group->linkObserver(observer);

			observer->traverse(parent);

			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&send_lane, remote_lane)
			}, helix::Dispatcher::global());
			COFIBER_AWAIT send_resp.future();
			COFIBER_AWAIT send_lane.future();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(send_lane.error());
		}else if(req.req_type() == managarm::mbus::CntReqType::BIND2) {
			helix::SendString<M> send_resp;
			helix::SendDescriptor<M> send_desc;

			auto entity = allEntities.at(req.id());
			if(typeid(*entity) != typeid(Object))
				throw std::runtime_error("Bind can only be invoked on objects");
			auto object = std::static_pointer_cast<Object>(entity);

			auto descriptor = COFIBER_AWAIT object->bind();
			
			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&send_desc, descriptor)
			}, helix::Dispatcher::global());
			COFIBER_AWAIT send_resp.future();
			COFIBER_AWAIT send_desc.future();
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

	serve(helix::UniquePipe(xpipe));

	while(true)
		helix::Dispatcher::global().dispatch();
}

