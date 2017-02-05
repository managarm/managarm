#ifndef THOR_GENERIC_FIBER_HPP
#define THOR_GENERIC_FIBER_HPP

namespace thor {

struct KernelFiber : ScheduleEntity {
	static void run(void (*function)());

	[[ noreturn ]] void invoke() override;

	explicit KernelFiber(AbiParameters abi);

private:
	FiberContext _context;
	Executor _executor;
};

} // namespace thor

#endif // THOR_GENERIC_FIBER_HPP
