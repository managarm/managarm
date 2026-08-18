#include <protocols/fs/file-locks.hpp>
#include <protocols/fs/server.hpp>
#include <fs.bragi.hpp>

namespace protocols::fs {
	Flock::~Flock() {
		if(manager == nullptr)
			return;

		bool notify = false;
		{
			std::lock_guard guard{manager->mutex};

			if(this->active) {
				manager->flocks.erase(manager->flocks.iterator_to(this));
				this->active = false;
				if(manager->flocks.empty())
					notify = true;
			}
		}

		if(notify)
			manager->flockNotify.raise();
	}

	async::result<protocols::fs::Error> FlockManager::lock(Flock *newFlock, int flags,
			async::cancellation_token ct) {
		if(!validateFlockFlags(flags))
			co_return protocols::fs::Error::illegalArguments;

		bool nonblock = flags & managarm::fs::FlockFlags::LOCK_NB;
		bool shared = flags & managarm::fs::FlockFlags::LOCK_SH;
		bool exclusive = flags & managarm::fs::FlockFlags::LOCK_EX;
		bool unlock = flags & managarm::fs::FlockFlags::LOCK_UN;

		if(unlock) {
			bool notify = false;
			{
				std::lock_guard guard{mutex};

				if(newFlock->active) {
					flocks.erase(flocks.iterator_to(newFlock));
					newFlock->manager = nullptr;
					newFlock->active = false;
					if(flocks.empty())
						notify = true;
				}
			}

			if(notify)
				flockNotify.raise();

			co_return protocols::fs::Error::none;
		}

		// Keep checking until there are no conflicts
		while (true) {
			bool conflict = false;
			// Note: conflicts implies !notify.
			bool notify = false;
			uint64_t seenSequence;
			{
				std::lock_guard guard{mutex};

				// TODO: Use async::sequenced_event::current_sequence() once it exists.
				seenSequence = flockNotify.next_sequence() - 1;

				for (auto f : flocks) {
					// Ignore our own existing lock (allows upgrade/downgrade)
					if (f == newFlock)
						continue;

					if (exclusive || f->type == protocols::fs::FLockState::LOCKED_EXCLUSIVE) {
						conflict = true;
						break;
					}
				}

				if (!conflict) {
					if (newFlock->active) {
						auto oldType = newFlock->type;
						newFlock->type = shared ? protocols::fs::FLockState::LOCKED_SHARED : protocols::fs::FLockState::LOCKED_EXCLUSIVE;
						// If downgraded from exclusive, notify other waiting locks
						if (oldType == protocols::fs::FLockState::LOCKED_EXCLUSIVE && shared)
							notify = true;
					} else {
						newFlock->type = shared ? protocols::fs::FLockState::LOCKED_SHARED : protocols::fs::FLockState::LOCKED_EXCLUSIVE;
						newFlock->manager = this;
						flocks.push_back(newFlock);
						newFlock->active = true;
					}
				}
			}

			if (conflict) {
				if (nonblock)
					co_return protocols::fs::Error::wouldBlock;

				co_await flockNotify.async_wait(seenSequence, ct);
				if (ct.is_cancellation_requested())
					co_return protocols::fs::Error::interrupted;
				continue;
			}

			if (notify)
				flockNotify.raise();

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
