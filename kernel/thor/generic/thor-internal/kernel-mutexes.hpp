#pragma once

#include <async/mutex.hpp>
#include <thor-internal/ipl.hpp>

namespace thor {

struct ExecutorContext;

// Mutex intended for very short critical sections.
// Requires preemption to be disabled but may block.
// Blocking at that IPL does not move the thread to another CPU (see ipl::noMigration).
// This should be used instead of a spinlock if blocking is required.
struct AdaptiveMutex {
public:
	struct Guard {
		Guard(AdaptiveMutex &mutex)
		: mutex_{&mutex} {
			mutex_->lock_();
		}

		~Guard() {
			mutex_->unlock_();
		}

		Guard(const Guard &) = delete;

		Guard &operator= (const Guard &) = delete;

	private:
		AdaptiveMutex *mutex_;
	};

private:
	void lock_();
	void lockSlow_();
	void unlock_();

	async::mutex asyncMutex_;
	std::atomic<ExecutorContext *> ownerCtx_;
};

} // namespace thor
