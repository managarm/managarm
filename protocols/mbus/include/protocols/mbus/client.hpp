
#ifndef LIBMBUS_MBUS_HPP
#define LIBMBUS_MBUS_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <unordered_map>

#include <async/result.hpp>
#include <boost/variant.hpp>
#include <cofiber.hpp>
#include <helix/ipc.hpp>

// EVENTUALLY: use std::variant instead of boost::variant!

namespace mbus {

namespace _detail {
	using EntityId = int64_t;

	struct Observer;
	struct AttachEvent;

	using AnyEvent = boost::variant<AttachEvent>;

	struct NoFilter;
	struct EqualsFilter;
	struct Conjunction;

	using AnyFilter = boost::variant<
		NoFilter,
		EqualsFilter,
		boost::recursive_wrapper<Conjunction>
	>;

	struct Connection {
		Connection(helix::Dispatcher *dispatcher, helix::UniqueLane lane)
		: dispatcher(dispatcher), lane(std::move(lane)) { }

		helix::Dispatcher *dispatcher;
		helix::UniqueLane lane;
	};
	
	// ------------------------------------------------------------------------
	// Properties.
	// ------------------------------------------------------------------------

	using Properties = std::unordered_map<std::string, std::string>;	
	
	// ------------------------------------------------------------------------
	// Filters.
	// ------------------------------------------------------------------------

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
	// mbus Instance class.
	// ------------------------------------------------------------------------

	struct Entity;

	struct Instance {
		static Instance global();
		
		Instance(helix::Dispatcher *dispatcher, helix::UniqueLane lane)
		: _connection(std::make_shared<Connection>(dispatcher, std::move(lane))) { }

		// attaches a root to the mbus.
		async::result<Entity> getRoot();

	private:
		std::shared_ptr<Connection> _connection;
	};
	
	// ------------------------------------------------------------------------
	// Entity related code.
	// ------------------------------------------------------------------------

	struct BindQuery { };

	using AnyQuery = boost::variant<BindQuery>;

	struct Entity {	
		explicit Entity(std::shared_ptr<Connection> connection, EntityId id)
		: _connection(std::move(connection)), _id(id) { }

		// creates a child group.
		async::result<Entity> createGroup(std::string name) const;

		// creates a child object.
		async::result<Entity> createObject(std::string name,
				const Properties &properties,
				std::function<async::result<helix::UniqueDescriptor>(AnyQuery)> handler) const;

		// links an observer to this group.
		async::result<Observer> linkObserver(const AnyFilter &filter,
				std::function<void(AnyEvent)> handler) const;

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
		explicit AttachEvent(Entity entity)
		: _entity(std::move(entity)) { }

		Entity getEntity() {
			return _entity;
		}

	private:
		Entity _entity;
	};

	struct Observer {
	};
}

using _detail::Instance;
using _detail::Properties;

using _detail::BindQuery;
using _detail::AnyQuery;
using _detail::Entity;

using _detail::NoFilter;
using _detail::EqualsFilter;
using _detail::Conjunction;
using _detail::AnyFilter;
using _detail::AttachEvent;
using _detail::AnyEvent;
using _detail::Observer;

} // namespace mbus

#endif // LIBMBUS_MBUS_HPP

