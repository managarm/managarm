
namespace frigg {
namespace atomic {

inline void barrier() {
	asm volatile ( "" : : : "memory" );
}

} } // namespace frigg::atomic

#if defined(__i386__)
#include "arch_x86/atomic_impl.hpp"
#elif defined(__x86_64__)
#include "arch_x86/atomic_impl.hpp"
#endif

namespace frigg {

struct NullLock {
	void lock() { }

	void unlock() { }
};

// FIXME: UGLY HACK
#ifndef ASSERT
#define ASSERT assert
#endif

struct DontLock { };

template<typename Mutex>
class LockGuard {
public:
	LockGuard(Mutex *mutex, DontLock dummy)
	: p_mutex(mutex), p_isLocked(false) { }

	LockGuard(Mutex *mutex)
	: p_mutex(mutex), p_isLocked(false) {
		lock();
	}

	LockGuard(const LockGuard &other) = delete;
	
	~LockGuard() {
		if(p_isLocked)
			unlock();
	}

	LockGuard &operator= (const LockGuard &other) = delete;

	void lock() {
		ASSERT(!p_isLocked);
		p_mutex->lock();
		p_isLocked = true;
	}

	void unlock() {
		ASSERT(p_isLocked);
		p_mutex->unlock();
		p_isLocked = false;
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
	return LockGuard<Mutex>();
}

} // namespace frigg

