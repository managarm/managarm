
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
namespace atomic {

struct NullLock {
	void lock() { }

	void unlock() { }
};

template<typename Mutex>
class LockGuard {
public:
	LockGuard(Mutex *mutex)
	: p_mutex(mutex) {
		p_mutex->lock();
	}

	LockGuard(const LockGuard &other) = delete;
	
	~LockGuard() {
		p_mutex->unlock();
	}

	LockGuard &operator= (const LockGuard &other) = delete;

private:
	Mutex *p_mutex;
};

} } // namespace frigg::atomic

