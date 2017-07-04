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
	IrqMutex()
	: _nesting{0} { }

	IrqMutex(const IrqMutex &) = delete;

	void lock() {
		// We maintain the following invariants:
		// * Properly nested lock()/unlock() pairs restore IRQs to the original state.
		// * If we observe _nesting > 0 then IRQs are disabled.
		auto n = _nesting.load(std::memory_order_relaxed);
		if(!n) {
			auto e = intsAreEnabled();
			if(e)
				disableInts();
			// NMIs/faults can interrupt us here but that is not a problem because of the first invariant.
			_nesting.fetch_add(1, std::memory_order_acq_rel); // TODO: Replace by store.
			_enabled = e;
		}else{
			// Because of the second invariant we do not need to examine the IRQ state here.
			_nesting.fetch_add(1, std::memory_order_acq_rel); // TODO: Replace by store.
		}
	}

	void unlock() {
		auto n = _nesting.fetch_sub(1, std::memory_order_acq_rel);
		assert(n);
		if(n == 1 && _enabled)
			enableInts();
	}

private:
	// TODO: We do not need 'lock xadd' here; 'xadd' alone suffices.
	std::atomic<unsigned int> _nesting;
	bool _enabled;
};

IrqMutex &irqMutex();

} // namespace thor

#endif // THOR_ARCH_X86_INTS_HPP
