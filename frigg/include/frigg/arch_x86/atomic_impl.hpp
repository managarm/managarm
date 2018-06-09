
#ifndef FRIGG_ARCH_X86_ATOMIC_IMPL_HPP
#define FRIGG_ARCH_X86_ATOMIC_IMPL_HPP

#include <frigg/macros.hpp>
#include <frigg/cxx-support.hpp>

namespace frigg FRIGG_VISIBILITY {

inline void barrier() {
	asm volatile ( "" : : : "memory" );
}

template<typename T, typename = void>
struct Atomic;

template<>
struct Atomic<int32_t> {
	static bool compareSwap(int32_t *pointer,
			int32_t expect, int32_t overwrite, int32_t &found) {
		bool success;
		asm volatile ( "lock cmpxchgl %3, %4\n"
					"\tsetz %0"
				: "=r" (success), "=a" (found)
				: "1" (expect), "r" (overwrite), "m" (*pointer) : "memory" );
		return success;
	}

	static void fetchInc(int32_t *pointer, int32_t &old_value) {
		asm volatile ( "lock xaddl %0, %1" : "=r" (old_value)
				: "m" (*pointer), "0" (1) : "memory" );
	}
	
	static void fetchDec(int32_t *pointer, int32_t &old_value) {
		asm volatile ( "lock xaddl %0, %1" : "=r" (old_value)
				: "m" (*pointer), "0" (-1) : "memory" );
	}
};

template<>
struct Atomic<uint32_t> {
	static void fetchInc(uint32_t *pointer, uint32_t &old_value) {
		asm volatile ( "lock xaddl %0, %1" : "=r" (old_value)
				: "m" (*pointer), "0" (1) : "memory" );
	}
};

template<>
struct Atomic<int64_t> {
	static void fetchInc(int64_t *pointer, int64_t &old_value) {
		asm volatile ( "lock xaddq %0, %1" : "=r" (old_value)
				: "m" (*pointer), "0" (1) : "memory" );
	}
};

template<typename T>
inline void volatileWrite(T *pointer, T value) {
	*const_cast<volatile T *>(pointer) = value;
}
template<typename T>
inline T volatileRead(T *pointer) {
	return *const_cast<volatile T *>(pointer);
}

inline void pause() {
	asm volatile ( "pause" );
}

template<typename T>
bool compareSwap(T *pointer, T expect, T overwrite, T &found) {
	return Atomic<T>::compareSwap(pointer, expect, overwrite, found);
}

template<typename T>
void fetchInc(T *pointer, T &old_value) {
	Atomic<T>::fetchInc(pointer, old_value);
}

template<typename T>
void fetchDec(T *pointer, T &old_value) {
	Atomic<T>::fetchDec(pointer, old_value);
}

class TicketLock {
public:
	TicketLock()
	: _nextTicket{0}, _servingTicket{0} { }

	TicketLock(const TicketLock &) = delete;

	TicketLock &operator= (const TicketLock &) = delete;

	void lock() {
		auto ticket = __atomic_fetch_add(&_nextTicket, 1, __ATOMIC_RELAXED);
		while(__atomic_load_n(&_servingTicket, __ATOMIC_ACQUIRE) != ticket) {
			pause();
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

#endif // FRIGG_ARCH_X86_ATOMIC_IMPL_HPP

