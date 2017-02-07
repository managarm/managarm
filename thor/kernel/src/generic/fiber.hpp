#ifndef THOR_GENERIC_FIBER_HPP
#define THOR_GENERIC_FIBER_HPP

#include <frigg/callback.hpp>
#include "../arch/x86/cpu.hpp"
#include "schedule.hpp"

namespace thor {

struct KernelFiber : ScheduleEntity {
	static void blockCurrent(frigg::CallbackPtr<bool()> predicate);

	static void run(void (*function)());

	explicit KernelFiber(AbiParameters abi);

	[[ noreturn ]] void invoke() override;

	void unblock();

private:
	bool _blocked;
	FiberContext _context;
	Executor _executor;
};

KernelFiber *thisFiber();

} // namespace thor

#endif // THOR_GENERIC_FIBER_HPP
