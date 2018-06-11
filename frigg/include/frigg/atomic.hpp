
#ifndef FRIGG_ATOMIC_HPP
#define FRIGG_ATOMIC_HPP

#include <frigg/algorithm.hpp>
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
	friend void swap(LockGuard &u, LockGuard &v) {
		swap(u._mutex, v._mutex);
		swap(u._isLocked, v._isLocked);
	}

	LockGuard()
	: _mutex{nullptr}, _isLocked{false} { }

	LockGuard(DontLock, Mutex *mutex)
	: _mutex{mutex}, _isLocked{false} { }

	LockGuard(Mutex *mutex)
	: _mutex{mutex}, _isLocked{false} {
		lock();
	}

	LockGuard(const LockGuard &other) = delete;
	
	LockGuard(LockGuard &&other)
	: LockGuard() {
		swap(*this, other);
	}

	~LockGuard() {
		if(_isLocked)
			unlock();
	}

	LockGuard &operator= (LockGuard other) {
		swap(*this, other);
		return *this;
	}

	void lock() {
		assert(!_isLocked);
		_mutex->lock();
		_isLocked = true;
	}

	void unlock() {
		assert(_isLocked);
		_mutex->unlock();
		_isLocked = false;
	}

	bool isLocked() {
		return _isLocked;
	}

	bool protects(Mutex *mutex) {
		return _isLocked && mutex == _mutex;
	}

private:
	Mutex *_mutex;
	bool _isLocked;
};

template<typename Mutex>
LockGuard<Mutex> guard(Mutex *mutex) {
	return LockGuard<Mutex>(mutex);
}

template<typename Mutex>
LockGuard<Mutex> guard(DontLock, Mutex *mutex) {
	return LockGuard<Mutex>(dontLock, mutex);
}

} // namespace frigg

#endif // FRIGG_ATOMIC_HPP

