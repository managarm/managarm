
#ifndef FRIGG_ATOMIC_HPP
#define FRIGG_ATOMIC_HPP

#include <frigg/macros.hpp>

#if defined(__i386__)
#include "arch_x86/atomic_impl.hpp"
#elif defined(__x86_64__)
#include "arch_x86/atomic_impl.hpp"
#endif

namespace frigg FRIGG_VISIBILITY {

struct NullLock {
	void lock() { }

	void unlock() { }
};

struct DontLock { };

constexpr DontLock dontLock = DontLock();

template<typename Mutex>
class LockGuard {
public:
	LockGuard(Mutex *mutex, DontLock)
	: p_mutex(mutex), p_isLocked(false) { }

	LockGuard(Mutex *mutex)
	: p_mutex(mutex), p_isLocked(false) {
		lock();
	}

	LockGuard(LockGuard &&other)
	: p_mutex(other.p_mutex), p_isLocked(other.p_isLocked) {
		other.p_isLocked = false;
	}

	LockGuard(const LockGuard &other) = delete;
	
	~LockGuard() {
		if(p_isLocked)
			unlock();
	}

	LockGuard &operator= (const LockGuard &other) = delete;

	void lock() {
		assert(!p_isLocked);
		p_mutex->lock();
		p_isLocked = true;
	}

	void unlock() {
		assert(p_isLocked);
		p_mutex->unlock();
		p_isLocked = false;
	}

	bool isLocked() {
		return p_isLocked;
	}

	bool protects(Mutex *mutex) {
		return p_isLocked && mutex == p_mutex;
	}

private:
	Mutex *p_mutex;
	bool p_isLocked;
};

template<typename Mutex>
LockGuard<Mutex> guard(Mutex *mutex) {
	return LockGuard<Mutex>(mutex);
}

} // namespace frigg

#endif // FRIGG_ATOMIC_HPP

