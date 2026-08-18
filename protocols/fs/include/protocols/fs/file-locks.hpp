#pragma once

#include <mutex>

#include <async/cancellation.hpp>
#include <async/sequenced-event.hpp>
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
		// Protected by mutex.
		FlockManager* manager = nullptr;
		// Protected by mutex.
		FLockState type;
		// Protected by mutex.
		frg::default_list_hook<Flock> hook_;
		// Protected by mutex.
		bool active = false;
	};

	struct FlockManager {
		friend struct Flock;
		async::result<protocols::fs::Error> lock(Flock* newFlock, int flags,
				async::cancellation_token ct = {});
private:
		std::mutex mutex;
		// Raised whenever a waiter may be able to make progress.
		async::sequenced_event flockNotify;

		// Protected by mutex.
		frg::intrusive_list<
			Flock,
			frg::locate_member<Flock, frg::default_list_hook<Flock>, &Flock::hook_>
		> flocks;

		static bool validateFlockFlags(int flags);
	};

}
