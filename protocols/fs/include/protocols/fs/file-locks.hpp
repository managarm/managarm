#pragma once

#include <protocols/fs/server.hpp>
#include <boost/intrusive/list.hpp>
#include <async/recurring-event.hpp>

namespace protocols::fs {

	enum FLockState {
		LOCKED_EXCLUSIVE = 0,
		LOCKED_SHARED = 1
	};

	struct FlockManager;

	typedef boost::intrusive::list_base_hook<boost::intrusive::link_mode<boost::intrusive::auto_unlink> > auto_unlink_hook;
	struct Flock : public boost::intrusive::list_base_hook<> {
		friend struct FlockManager;
		Flock(FLockState t, FlockManager* m) : manager(m), type(t) {}
		Flock() : manager(nullptr), type(FLockState::LOCKED_EXCLUSIVE) {}
		~Flock();

private:
		FlockManager* manager = nullptr;
		FLockState type;

	};

	struct FlockManager {
		friend struct Flock;
		async::result<protocols::fs::Error> lock(Flock* newFlock, int flags);
private:
		boost::intrusive::list<Flock> flocks;
		async::recurring_event flockNotify;
		static bool validateFlockFlags(int flags);
	};

}
