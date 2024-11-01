#include <fs.bragi.hpp>
#include <iostream>
#include <protocols/fs/file-locks.hpp>
#include <protocols/fs/server.hpp>

namespace protocols::fs {
Flock::~Flock() {
	if (manager != nullptr) {
		if (this->is_linked()) {
			auto iter = boost::intrusive::list<Flock>::s_iterator_to(*this);
			manager->flocks.erase(iter);
			if (manager->flocks.size() == 0) {
				manager->flockNotify.raise();
			}
		}
	}
}

async::result<protocols::fs::Error> FlockManager::lock(Flock *newFlock, int flags) {
	bool nonblock = flags & managarm::fs::FlockFlags::LOCK_NB;
	bool shared = flags & managarm::fs::FlockFlags::LOCK_SH;
	if (shared) {
		newFlock->type = protocols::fs::FLockState::LOCKED_SHARED;
	}

	if (flags & managarm::fs::FlockFlags::LOCK_UN) {
		if (newFlock->is_linked()) {
			flocks.clear();
			flockNotify.raise();
		}
		co_return protocols::fs::Error::none;
	}

	for (auto it = flocks.begin(); it != flocks.end();) {
		if ((*it).type == protocols::fs::FLockState::LOCKED_EXCLUSIVE) {
			if (nonblock) {
				co_return protocols::fs::Error::wouldBlock;
			}
			co_await flockNotify.async_wait();

			flocks.push_back(*newFlock);
			newFlock->manager = this;
			co_return protocols::fs::Error::none;
		} else {
			if (!shared) {
				if (nonblock) {
					co_return protocols::fs::Error::wouldBlock;
				}
				co_await flockNotify.async_wait();

				flocks.push_back(*newFlock);
				newFlock->manager = this;
				co_return protocols::fs::Error::none;
			} else {
				flocks.push_back(*newFlock);
				newFlock->manager = this;
				co_return protocols::fs::Error::none;
			}
		}
	}
	flocks.push_back(*newFlock);
	newFlock->manager = this;
	co_return protocols::fs::Error::none;
}

bool FlockManager::validateFlockFlags(int flags) {
	if (flags & managarm::fs::FlockFlags::LOCK_SH) {
		if (flags & managarm::fs::FlockFlags::LOCK_EX) {
			return false;
		} else if (flags & managarm::fs::FlockFlags::LOCK_UN) {
			return false;
		}
	} else if (flags & managarm::fs::FlockFlags::LOCK_EX) {
		if (flags & managarm::fs::FlockFlags::LOCK_SH) {
			return false;
		} else if (flags & managarm::fs::FlockFlags::LOCK_UN) {
			return false;
		}
	} else if (flags > 0b1111) {
		return false;
	}

	return true;
}
} // namespace protocols::fs
