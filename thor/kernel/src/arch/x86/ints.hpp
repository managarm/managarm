#ifndef THOR_ARCH_X86_INTS_HPP
#define THOR_ARCH_X86_INTS_HPP

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

struct GlobalIrqMutex { };
static constexpr GlobalIrqMutex globalIrqMutex;

struct IrqLock {
	IrqLock(GlobalIrqMutex)
	: _locked{true} {
		_enable = intsAreEnabled();
		if(_enable)
			disableInts();
	}

	IrqLock(const IrqLock &) = delete;

	~IrqLock() {
		if(_enable)
			enableInts();
	}

	IrqLock &operator= (const IrqLock &) = delete;

private:
	bool _locked;
	bool _enable;
};

} // namespace thor

#endif // THOR_ARCH_X86_INTS_HPP
