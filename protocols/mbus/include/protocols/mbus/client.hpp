#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unordered_set>
#include <unordered_map>
#include <variant>
#include <vector>
#include <memory>
#include <optional>

#include <async/result.hpp>
#include <helix/ipc.hpp>

#include <frg/expected.hpp>

namespace mbus_ng {

using EntityId = int64_t;

// ------------------------------------------------------------------------
// Filters.
// ------------------------------------------------------------------------

struct NoFilter;
struct EqualsFilter;
struct Conjunction;
struct Disjunction;

using AnyFilter = std::variant<
	NoFilter,
	EqualsFilter,
	Conjunction,
	Disjunction
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
	Conjunction(std::vector<AnyFilter> &&operands);

	const std::vector<AnyFilter> &operands() const &;

private:
	std::vector<AnyFilter> operands_;
};

struct Disjunction {
	Disjunction(std::vector<AnyFilter> &&operands);

	const std::vector<AnyFilter> &operands() const &;

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
	std::string name;
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
	async::result<Error> updateProperties(Properties properties);

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
