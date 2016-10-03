
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
			std::unordered_map<std::string, std::string> descriptor)
	: _id(id), _parent(std::move(parent)), _descriptor(std::move(descriptor)) { }

	int64_t getId() const {
		return _id;
	}

	std::shared_ptr<Group> getParent() const {
		return _parent.lock();
	}

	const std::unordered_map<std::string, std::string> &getDescriptor() const {
		return _descriptor;
	}

	virtual void traverse(std::vector<std::shared_ptr<Entity>> &descendants) = 0;

	virtual void linkObserver(std::shared_ptr<Observer> observer) = 0;

private:
	int64_t _id;
	std::weak_ptr<Group> _parent;
	std::unordered_map<std::string, std::string> _descriptor;
};

struct Group : Entity {
	explicit Group(int64_t id, std::weak_ptr<Group> parent,
			std::unordered_map<std::string, std::string> descriptor)
	: Entity(id, std::move(parent), std::move(descriptor)) { }
	
	void traverse(std::vector<std::shared_ptr<Entity>> &descendants) override {
		for(std::shared_ptr<Entity> child : _children) {
			descendants.push_back(std::move(child));
			child->traverse(descendants);
		}
	}

	void linkObserver(std::shared_ptr<Observer> observer) override {
		_observers.insert(std::move(observer));
	}

	void observeAttach(std::shared_ptr<Entity> entity);

private:
	std::unordered_set<std::shared_ptr<Entity>> _children;
	std::unordered_set<std::shared_ptr<Observer>> _observers;
};

struct Object : Entity {
	explicit Object(int64_t id, std::weak_ptr<Group> parent,
			std::unordered_map<std::string, std::string> descriptor,
			helix::UniquePipe lane)
	: Entity(id, std::move(parent), std::move(descriptor)),
			_lane(std::move(lane)) { }

	void traverse(std::vector<std::shared_ptr<Entity>> &descendants) override {
		// we don't have to do anything here
	}

	void linkObserver(std::shared_ptr<Observer> observer) override {
		throw std::runtime_error("Cannot attach observer to Object");
	}

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
	std::cout << "mbus: Object::bind() returns" << std::endl;
	
	COFIBER_RETURN(recv_desc.descriptor());
}))

struct EqualsFilter {
	EqualsFilter(std::string property, std::string value)
	: _property(std::move(property)), _value(std::move(value)) { }

	std::string getProperty() const { return _property; }
	std::string getValue() const { return _value; }

private:
	std::string _property;
	std::string _value;
};

struct Observer {
	explicit Observer(EqualsFilter filter, helix::UniquePipe lane)
	: _filter(std::move(filter)), _lane(std::move(lane)) { }

	cofiber::no_future observeAttach(std::shared_ptr<Entity> entity);

private:
	EqualsFilter _filter;
	helix::UniquePipe _lane;
};

static bool matchesFilter(const Entity *entity, const EqualsFilter &filter) {
	auto &properties = entity->getDescriptor();
	auto it = properties.find(filter.getProperty());
	if(it == properties.end())
		return false;
	return it->second == filter.getValue();
}
	
void Group::observeAttach(std::shared_ptr<Entity> entity) {
	for(auto &observer_ptr : _observers)
		observer_ptr->observeAttach(entity);
}

COFIBER_ROUTINE(cofiber::no_future, Observer::observeAttach(std::shared_ptr<Entity> entity), ([=] {
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

static EqualsFilter decodeFilter(const managarm::mbus::AnyFilter &msg) {
	if(msg.type_case() == managarm::mbus::AnyFilter::kEqualsFilter) {
		return EqualsFilter(msg.equals_filter().path(),
				msg.equals_filter().value());
	}else{
		throw std::runtime_error("Unexpected filter message");
	}
}

COFIBER_ROUTINE(cofiber::no_future, serve(helix::UniquePipe p),
		([pipe = std::move(p)] () {
	using M = helix::AwaitMechanism;

	while(true) {
		char buffer[256];
		helix::RecvString<M> recv_req(helix::Dispatcher::global(), pipe,
				buffer, 256, 0, 0, kHelRequest);
		COFIBER_AWAIT recv_req.future();
		HEL_CHECK(recv_req.error());

		managarm::mbus::CntRequest req;
		req.ParseFromArray(buffer, recv_req.actualLength());
		if(req.req_type() == managarm::mbus::CntReqType::GET_ROOT) {
			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);
			resp.set_id(1);

			auto serialized = resp.SerializeAsString();
			helix::SendString<M> send_resp(helix::Dispatcher::global(), pipe,
					serialized.data(), serialized.size(), 0, 0, kHelResponse);
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
		}else if(req.req_type() == managarm::mbus::CntReqType::CREATE_OBJECT) {
			auto parent = allEntities.at(req.parent_id());
			std::unordered_map<std::string, std::string> descriptor;
			for(auto &kv : req.descriptor().fields())
				descriptor.insert({ kv.first, kv.second.string() });

			helix::UniquePipe local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createFullPipe();
			// FIXME: this cast is not safe
			auto entity = std::make_shared<Object>(nextEntityId++,
					std::static_pointer_cast<Group>(parent), std::move(descriptor),
					std::move(local_lane));
			allEntities.insert({ entity->getId(), entity });

			std::shared_ptr<Group> current = std::static_pointer_cast<Group>(parent);
			while(current) {
				current->observeAttach(entity);
				// FIXME: this cast is not safe
				current = std::static_pointer_cast<Group>(current->getParent());
			}

			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);
			resp.set_id(entity->getId());

			auto serialized = resp.SerializeAsString();
			helix::SendString<M> send_resp(helix::Dispatcher::global(), pipe,
					serialized.data(), serialized.size(), 0, 0, kHelResponse);
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
			
			helix::SendDescriptor<M> send_lane(helix::Dispatcher::global(), pipe,
					remote_lane, 0, 0, kHelResponse);
			COFIBER_AWAIT send_lane.future();
			HEL_CHECK(send_lane.error());
		}else if(req.req_type() == managarm::mbus::CntReqType::LINK_OBSERVER) {
			auto entity = allEntities.at(req.id());

			helix::UniquePipe local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createFullPipe();
			auto observer = std::make_shared<Observer>(decodeFilter(req.filter()),
					std::move(local_lane));
			entity->linkObserver(observer);

			std::vector<std::shared_ptr<Entity>> descendants;
			for(auto it = descendants.begin(); it != descendants.end(); ++it)
				observer->observeAttach(std::move(*it));

			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);
			resp.set_id(entity->getId());

			auto serialized = resp.SerializeAsString();
			helix::SendString<M> send_resp(helix::Dispatcher::global(), pipe,
					serialized.data(), serialized.size(), 0, 0, kHelResponse);
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
			
			helix::SendDescriptor<M> send_lane(helix::Dispatcher::global(), pipe,
					remote_lane, 0, 0, kHelResponse);
			COFIBER_AWAIT send_lane.future();
			HEL_CHECK(send_lane.error());
		}else if(req.req_type() == managarm::mbus::CntReqType::BIND2) {
			auto entity = allEntities.at(req.id());
			// FIXME: this cast is not typesafe
			auto object = std::static_pointer_cast<Object>(entity);

			auto descriptor = COFIBER_AWAIT object->bind();
			
			managarm::mbus::SvrResponse resp;
			resp.set_error(managarm::mbus::Error::SUCCESS);

			auto serialized = resp.SerializeAsString();
			helix::SendString<M> send_resp(helix::Dispatcher::global(), pipe,
					serialized.data(), serialized.size(), 0, 0, kHelResponse);
			COFIBER_AWAIT send_resp.future();
			HEL_CHECK(send_resp.error());
			
			helix::SendDescriptor<M> send_desc(helix::Dispatcher::global(), pipe,
					descriptor, 0, 0, kHelResponse);
			COFIBER_AWAIT send_desc.future();
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

	auto entity = std::make_shared<Group>(nextEntityId++, std::weak_ptr<Group>(),
			std::unordered_map<std::string, std::string>());
	allEntities.insert({ entity->getId(), entity });

	unsigned long xpipe;
	if(peekauxval(AT_XPIPE, &xpipe))
		throw std::runtime_error("No AT_XPIPE specified");

	serve(helix::UniquePipe(xpipe));

	while(true)
		helix::Dispatcher::global().dispatch();
}

