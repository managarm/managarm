#pragma once

#include <map>
#include <protocols/mbus/client.hpp>

#include "controller.hpp"
#include "namespace.hpp"

namespace nvme {

struct Subsystem {
	Subsystem() {}

	async::result<void> run();

	mbus_ng::EntityId id() const {
		assert(mbusEntity_);
		return mbusEntity_->id();
	}

	void addController(mbus_ng::EntityId id, std::unique_ptr<Controller> c) {
		controllers_.insert({id, std::move(c)});
	}

	std::map<mbus_ng::EntityId, std::unique_ptr<Controller>> &controllers() {
		return controllers_;
	}

private:
	std::map<mbus_ng::EntityId, std::unique_ptr<Controller>> controllers_ = {};
	std::unique_ptr<mbus_ng::EntityManager> mbusEntity_;
};

} // namespace nvme
