#include <protocols/fs/file-locks.hpp>
#include <protocols/fs/server.hpp>
#include <fs.bragi.hpp>

namespace protocols::fs {
	Flock::~Flock() {
		if(manager != nullptr) {
			if(this->active) {
				manager->flocks.erase(manager->flocks.iterator_to(this));
				if(manager->flocks.empty())
					manager->flockNotify.raise();
			}
		}
	}

	// TODO: take an async::cancellation_token to support interruption
	async::result<protocols::fs::Error> FlockManager::lock(Flock *newFlock, int flags) {
		if(!validateFlockFlags(flags))
			co_return protocols::fs::Error::illegalArguments;

		bool nonblock = flags & managarm::fs::FlockFlags::LOCK_NB;
		bool shared = flags & managarm::fs::FlockFlags::LOCK_SH;
		bool exclusive = flags & managarm::fs::FlockFlags::LOCK_EX;
		bool unlock = flags & managarm::fs::FlockFlags::LOCK_UN;

		if(unlock) {
			if(newFlock->active) {
				flocks.erase(flocks.iterator_to(newFlock));
				newFlock->manager = nullptr;
				newFlock->active = false;
				if(flocks.empty())
					flockNotify.raise();
			}

			co_return protocols::fs::Error::none;
		}

		// Keep checking until there are no conflicts
		while (true) {
			bool conflict = false;
			for (auto f : flocks) {
				// Ignore our own existing lock (allows upgrade/downgrade)
				if (f == newFlock)
					continue;

				if (exclusive || f->type == protocols::fs::FLockState::LOCKED_EXCLUSIVE) {
					conflict = true;
					break;
				}
			}

			if (conflict) {
				if (nonblock)
					co_return protocols::fs::Error::wouldBlock;

				co_await flockNotify.async_wait();
				continue;
			}

			if (newFlock->active) {
				auto oldType = newFlock->type;
				newFlock->type = shared ? protocols::fs::FLockState::LOCKED_SHARED : protocols::fs::FLockState::LOCKED_EXCLUSIVE;
				// If downgraded from exclusive, notify other waiting locks
				if (oldType == protocols::fs::FLockState::LOCKED_EXCLUSIVE && shared)
					flockNotify.raise();
			} else {
				newFlock->type = shared ? protocols::fs::FLockState::LOCKED_SHARED : protocols::fs::FLockState::LOCKED_EXCLUSIVE;
				newFlock->manager = this;
				flocks.push_back(newFlock);
				newFlock->active = true;
			}

			co_return protocols::fs::Error::none;
		}
	}

	bool FlockManager::validateFlockFlags(int flags) {
		constexpr int flockOps = managarm::fs::FlockFlags::LOCK_SH
			| managarm::fs::FlockFlags::LOCK_EX
			| managarm::fs::FlockFlags::LOCK_UN;

		constexpr int validFlockFlags = flockOps | managarm::fs::FlockFlags::LOCK_NB;

		if (flags & ~validFlockFlags)
			return false;

		// Exactly one of LOCK_SH, LOCK_EX, LOCK_UN must be specified
		int op = flags & flockOps;
		return op == managarm::fs::FlockFlags::LOCK_SH
			|| op == managarm::fs::FlockFlags::LOCK_EX
			|| op == managarm::fs::FlockFlags::LOCK_UN;
	}
}
