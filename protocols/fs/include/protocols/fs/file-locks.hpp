#pragma once

#include <async/recurring-event.hpp>
#include <frg/intrusive.hpp>
#include <protocols/fs/server.hpp>

namespace protocols::fs {

	enum FLockState {
		LOCKED_EXCLUSIVE = 0,
		LOCKED_SHARED = 1
	};

	struct FlockManager;

	struct Flock {
		friend struct FlockManager;
		Flock(FLockState t, FlockManager* m) : manager(m), type(t) {}
		Flock() : manager(nullptr), type(FLockState::LOCKED_EXCLUSIVE) {}
		~Flock();

private:
		FlockManager* manager = nullptr;
		FLockState type;
		frg::default_list_hook<Flock> hook_;
	};

	struct FlockManager {
		friend struct Flock;
		async::result<protocols::fs::Error> lock(Flock* newFlock, int flags);
private:
		frg::intrusive_list<
			Flock,
			frg::locate_member<Flock, frg::default_list_hook<Flock>, &Flock::hook_>
		> flocks;
		async::recurring_event flockNotify;
		static bool validateFlockFlags(int flags);
	};

}
