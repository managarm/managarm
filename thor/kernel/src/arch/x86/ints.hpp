#ifndef THOR_ARCH_X86_INTS_HPP
#define THOR_ARCH_X86_INTS_HPP

#include <atomic>

namespace thor {

void initializeProcessorEarly();

void setupIdt(uint32_t *table);

bool intsAreEnabled();

void enableInts();

void disableInts();

inline void halt() {
	asm volatile ( "hlt" );
}

void suspendSelf();

// this is used to enter user mode in the user_boot thread
// we do not need it inside other threads
extern "C" void enterUserMode(void *stack_ptr, void *ip) __attribute__ (( noreturn ));

struct IrqMutex {
private:
	static constexpr unsigned int enableBit = 0x8000'0000;

public:
	IrqMutex()
	: _state{0} { }

	IrqMutex(const IrqMutex &) = delete;

	void lock() {
		// We maintain the following invariants:
		// * Properly nested lock()/unlock() pairs restore IRQs to the original state.
		// * If we observe _state > 0 then IRQs are disabled.
		//
		// NMIs and faults can always interrupt us but that is
		// not a problem because of the first invariant.
		auto s = _state.load(std::memory_order_acquire);
		if(!s) {
			auto e = intsAreEnabled();
			if(e) {
				disableInts();
				_state.store(enableBit | 1, std::memory_order_relaxed);
			}else{
				_state.store(1, std::memory_order_relaxed);
			}
		}else{
			// Because of the second invariant we do not need to examine the IRQ state here.
			assert(s & ~enableBit);
			_state.store(s + 1, std::memory_order_release);
		}
	}

	void unlock() {
		auto s = _state.load(std::memory_order_relaxed);
		assert(s & ~enableBit);
		if((s & ~enableBit) == 1) {
			_state.store(0, std::memory_order_release);
			if(s & enableBit)
				enableInts();
		}else{
			_state.store(s - 1, std::memory_order_release);
		}
	}

	unsigned int nesting() {
		return _state.load(std::memory_order_relaxed) & ~enableBit;
	}

private:
	std::atomic<unsigned int> _state;
};

struct StatelessIrqLock {
	StatelessIrqLock()
	: _locked{false} {
		lock();
	}

	StatelessIrqLock(frigg::DontLock)
	: _locked{false} { }

	StatelessIrqLock(const StatelessIrqLock &) = delete;

	~StatelessIrqLock() {
		if(_locked)
			unlock();
	}

	StatelessIrqLock &operator= (const StatelessIrqLock &) = delete;

	void lock() {
		assert(!_locked);
		_enabled = intsAreEnabled();
		disableInts();
		_locked = true;
	}

	void unlock() {
		assert(_locked);
		if(_enabled)
			enableInts();
		_locked = false;
	}

private:
	bool _locked;
	bool _enabled;
};

IrqMutex &irqMutex();

} // namespace thor

#endif // THOR_ARCH_X86_INTS_HPP
