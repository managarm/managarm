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
