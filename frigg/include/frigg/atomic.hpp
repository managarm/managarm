
#ifndef FRIGG_ATOMIC_HPP
#define FRIGG_ATOMIC_HPP

#include <frigg/macros.hpp>
#include <frigg/cxx-support.hpp>

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
		using std::swap;
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

class TicketLock {
public:
	constexpr TicketLock()
	: _nextTicket{0}, _servingTicket{0} { }

	TicketLock(const TicketLock &) = delete;

	TicketLock &operator= (const TicketLock &) = delete;

	void lock() {
		auto ticket = __atomic_fetch_add(&_nextTicket, 1, __ATOMIC_RELAXED);
		while(__atomic_load_n(&_servingTicket, __ATOMIC_ACQUIRE) != ticket) {
#ifdef __x86_64__
			asm volatile ("pause");
#endif
		}
	}

	void unlock() {
		auto current = __atomic_load_n(&_servingTicket, __ATOMIC_RELAXED);
		__atomic_store_n(&_servingTicket, current + 1, __ATOMIC_RELEASE);
	}

private:
	uint32_t _nextTicket;
	uint32_t _servingTicket;
};

} // namespace frigg

#endif // FRIGG_ATOMIC_HPP

