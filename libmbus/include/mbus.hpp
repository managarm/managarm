
#ifndef LIBMBUS_MBUS_HPP
#define LIBMBUS_MBUS_HPP

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <helix/ipc.hpp>
#include <boost/variant.hpp>
#include <cofiber.hpp>
#include <cofiber/future.hpp>

// EVENTUALLY: use std::future instead of cofiber::future!
// EVENTUALLY: use std::variant instead of boost::variant!

namespace mbus {

namespace _detail {
	using EntityId = int64_t;

	struct Internal {
		Internal(helix::Dispatcher dispatcher, helix::UniquePipe pipe)
		: dispatcher(std::move(dispatcher)), pipe(std::move(pipe)) { }

		helix::Dispatcher dispatcher;
		helix::UniquePipe pipe;
	};
	
	// ------------------------------------------------------------------------
	// Properties.
	// ------------------------------------------------------------------------

	struct Properties {

	};
	
	// ------------------------------------------------------------------------
	// Filters.
	// ------------------------------------------------------------------------

	struct PropertyFilter { };

	using AnyFilter = boost::variant<PropertyFilter>;
	
	// ------------------------------------------------------------------------
	// mbus Instance class.
	// ------------------------------------------------------------------------

	struct Entity;

	struct Instance {
		static Instance global();
		
		Instance(helix::Dispatcher dispatcher, helix::UniquePipe pipe)
		: _internal(std::make_shared<Internal>(std::move(dispatcher), std::move(pipe))) { }

		// attaches a root to the mbus.
		cofiber::future<Entity> getRoot();

	private:
		std::shared_ptr<Internal> _internal;
	};
	
	// ------------------------------------------------------------------------
	// Observer related code.
	// ------------------------------------------------------------------------

	struct AttachEvent { };

	using AnyEvent = boost::variant<AttachEvent>;

	struct Observer {
	};
	
	// ------------------------------------------------------------------------
	// Entity related code.
	// ------------------------------------------------------------------------

	struct ConnectQuery { };

	using AnyQuery = boost::variant<ConnectQuery>;

	struct Entity {	
		// attaches a child group.
		cofiber::future<Entity> attachGroup(std::string name);

		// attaches a child object.
		cofiber::future<Entity> attachObject(std::string name,
				const Properties &properties,
				std::function<void(AnyQuery)> handler);

		// links an observer to this group.
		cofiber::future<Observer> linkObserver(const AnyFilter &filter,
				std::function<void(AnyEvent)> handler);

	private:
	};
}

using _detail::Instance;

} // namespace mbus

#endif // LIBMBUS_MBUS_HPP

