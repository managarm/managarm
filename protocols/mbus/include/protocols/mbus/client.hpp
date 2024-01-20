#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unordered_map>
#include <variant>
#include <vector>
#include <functional>
#include <memory>
#include <optional>

#include <async/result.hpp>
#include <helix/ipc.hpp>


namespace mbus {

namespace _detail {
	using EntityId = int64_t;

	// ------------------------------------------------------------------------
	// Filters.
	// ------------------------------------------------------------------------

	struct NoFilter;
	struct EqualsFilter;
	struct Conjunction;

	using AnyFilter = std::variant<
		NoFilter,
		EqualsFilter,
		Conjunction
	>;

	struct NoFilter { };

	struct EqualsFilter {
		EqualsFilter(std::string path, std::string value)
		: _path(std::move(path)), _value(std::move(value)) { }

		std::string getPath() const { return _path; }
		std::string getValue() const { return _value; }

	private:
		std::string _path;
		std::string _value;
	};

	struct Conjunction {
		Conjunction(std::vector<AnyFilter> operands)
		: _operands(std::move(operands)) { }

		const std::vector<AnyFilter> &getOperands() const { return _operands; }

	private:
		std::vector<AnyFilter> _operands;
	};

	// ------------------------------------------------------------------------
	// Properties.
	// ------------------------------------------------------------------------

	struct StringItem;
	struct ListItem;

	using AnyItem = std::variant<
		StringItem,
		ListItem
	>;

	struct StringItem {
		std::string value;
	};

	struct ListItem {

	};

	using Properties = std::unordered_map<std::string, AnyItem>;

	// ------------------------------------------------------------------------
	// Private state object.
	// ------------------------------------------------------------------------

	struct Connection {
		Connection(helix::UniqueLane lane)
		: lane(std::move(lane)) { }

		struct EnumeratedEntity {
			EntityId id;
			Properties properties;
		};

		struct EnumerationResult {
			uint64_t outSeq;
			uint64_t actualSeq;
			std::vector<EnumeratedEntity> entities;
		};

		async::result<EnumerationResult> enumerate(uint64_t seq, const AnyFilter &filter) const;

		helix::UniqueLane lane;
	};

	// ------------------------------------------------------------------------
	// mbus Instance class.
	// ------------------------------------------------------------------------

	struct Entity;

	struct Instance {
		static Instance global();

		Instance(helix::UniqueLane lane)
		: _connection(std::make_shared<Connection>(std::move(lane))) { }

		// Returns the mbus root entity.
		async::result<Entity> getRoot();

		// Returns an mbus entity given its ID.
		async::result<Entity> getEntity(int64_t id);

	private:
		std::shared_ptr<Connection> _connection;
	};

	// ------------------------------------------------------------------------
	// Entity related code.
	// ------------------------------------------------------------------------

	struct Entity;
	struct Observer;

	struct ObjectHandler {
		ObjectHandler &withBind(std::function<async::result<helix::UniqueDescriptor>()> f) {
			bind = std::move(f);
			return *this;
		}

		std::function<async::result<helix::UniqueDescriptor>()> bind;
	};

	struct ObserverHandler {
		ObserverHandler &withAttach(std::function<void(Entity, Properties)> f) {
			attach = std::move(f);
			return *this;
		}

		std::function<void(Entity, Properties)> attach;
	};

	struct Entity {
		explicit Entity(std::shared_ptr<Connection> connection, EntityId id)
		: _connection(std::move(connection)), _id(id) { }

		EntityId getId() const {
			return _id;
		}

		async::result<Properties> getProperties() const;

		// creates a child group.
		async::result<Entity> createGroup(std::string name) const;

		// creates a child object.
		async::result<Entity> createObject(std::string name,
				const Properties &properties, ObjectHandler handler) const;

		// links an observer to this group.
		async::result<Observer> linkObserver(const AnyFilter &filter,
				ObserverHandler handler) const;

		// bind to the device.
		async::result<helix::UniqueDescriptor> bind() const;

	private:
		std::shared_ptr<Connection> _connection;
		EntityId _id;
	};

	// ------------------------------------------------------------------------
	// Observer related code.
	// ------------------------------------------------------------------------

	struct AttachEvent {
		explicit AttachEvent(Entity entity, Properties properties)
		: _entity{std::move(entity)}, _properties{std::move(properties)} { }

		Entity getEntity() {
			return _entity;
		}

		const Properties &getProperties() {
			return _properties;
		}

	private:
		Entity _entity;
		Properties _properties;
	};

	struct Observer {
	};
}

using _detail::NoFilter;
using _detail::EqualsFilter;
using _detail::Conjunction;
using _detail::AnyFilter;

using _detail::StringItem;
using _detail::ListItem;
using _detail::Properties;

using _detail::ObjectHandler;
using _detail::ObserverHandler;
using _detail::Instance;
using _detail::Entity;
using _detail::Observer;

void recreateInstance();

} // namespace mbus

#include <frg/expected.hpp>
#include <unordered_set>

namespace mbus_ng {

using EntityId = int64_t;

// ------------------------------------------------------------------------
// Filters.
// ------------------------------------------------------------------------

struct NoFilter;
struct EqualsFilter;
struct Conjunction;

using AnyFilter = std::variant<
	NoFilter,
	EqualsFilter,
	Conjunction
>;

struct NoFilter { };

struct EqualsFilter {
	EqualsFilter(std::string path, std::string value)
	: path_{std::move(path)}, value_{std::move(value)} { }

	const std::string &path() const & { return path_; }
	const std::string &value() const & { return value_; }

private:
	std::string path_;
	std::string value_;
};

struct Conjunction {
	Conjunction(std::vector<AnyFilter> &&operands)
	: operands_{std::move(operands)} { }

	const std::vector<AnyFilter> &operands() const & { return operands_; }

private:
	std::vector<AnyFilter> operands_;
};

// ------------------------------------------------------------------------
// Properties.
// ------------------------------------------------------------------------

struct StringItem;
struct ListItem;

using AnyItem = std::variant<
	StringItem
>;

struct StringItem {
	std::string value;
};

using Properties = std::unordered_map<std::string, AnyItem>;

// ------------------------------------------------------------------------
// Private state object.
// ------------------------------------------------------------------------

struct Connection {
	Connection(helix::UniqueLane lane)
	: lane(std::move(lane)) { }

	helix::UniqueLane lane;
};

// ------------------------------------------------------------------------
// Errors.
// ------------------------------------------------------------------------

enum class Error {
	success,
	protocolViolation,
	noSuchEntity,
};

template <typename T>
using Result = frg::expected<Error, T>;

// ------------------------------------------------------------------------
// mbus Enumerator class.
// ------------------------------------------------------------------------

struct EnumerationEvent {
	enum class Type {
		created,
		propertiesChanged,
		removed
	} type;

	EntityId id;
	Properties properties;
};

struct EnumerationResult {
	bool paginated;
	std::vector<EnumerationEvent> events;
};

struct Enumerator {
	Enumerator(std::shared_ptr<Connection> connection, AnyFilter &&filter)
	: connection_{connection}, filter_{std::move(filter)} { }

	// Get changes since last enumeration
	async::result<Result<EnumerationResult>> nextEvents();

private:
	std::shared_ptr<Connection> connection_;
	AnyFilter filter_;

	uint64_t curSeq_ = 0;
	std::unordered_set<EntityId> seenIds_{};
};

// ------------------------------------------------------------------------
// mbus Instance class.
// ------------------------------------------------------------------------

struct Entity;
struct EntityManager;

struct Instance {
	static Instance global();

	Instance(helix::UniqueLane lane)
	: connection_{std::make_shared<Connection>(std::move(lane))} { }

	async::result<Entity> getEntity(EntityId id);

	async::result<Result<EntityManager>> createEntity(std::string_view name, const Properties &properties);

	Enumerator enumerate(AnyFilter filter) {
		return {connection_, std::move(filter)};
	}

private:
	std::shared_ptr<Connection> connection_;
};

// ------------------------------------------------------------------------
// mbus Entity class.
// ------------------------------------------------------------------------

struct Entity {
	Entity(std::shared_ptr<Connection> connection, EntityId id)
	: connection_{connection}, id_{id} { }

	EntityId id() const {
		return id_;
	}

	async::result<Result<Properties>> getProperties() const;
	async::result<Result<helix::UniqueLane>> getRemoteLane() const;

private:
	std::shared_ptr<Connection> connection_;

	EntityId id_;
};

// ------------------------------------------------------------------------
// mbus EntityManager class.
// ------------------------------------------------------------------------

struct EntityManager {
	EntityManager(EntityId id, helix::UniqueLane mgmtLane)
	: id_{id}, mgmtLane_{std::move(mgmtLane)} { }

	~EntityManager() {
		// TODO(qookie): Allow destroying entities. This
		// requires support in the mbus server, since it needs
		// to cancel any pending operations, destroy the
		// entity, and notify enumerators.
		assert(!mgmtLane_ && "FIXME: tried to destroy entity");
	}

	// NOTE(qookie): Getting rid of ~EntityManager will let us get
	// rid of these too.
	EntityManager(const EntityManager &other) = delete;
	EntityManager(EntityManager &&other) = default;

	EntityId id() const {
		return id_;
	}

	async::result<Entity> intoEntity() const {
		co_return co_await mbus_ng::Instance::global().getEntity(id());
	}

	// Serves the remote lane to one client. Completes only after the lane is consumed.
	async::result<Result<void>> serveRemoteLane(helix::UniqueLane lane) const;

private:
	EntityId id_;
	helix::UniqueLane mgmtLane_;
};

void recreateInstance();

} // namespace mbus_ng
