
#ifndef HEL_SYSCALLS_H
#define HEL_SYSCALLS_H

typedef uint64_t HelWord;
typedef HelWord HelSyscallInput[9];
typedef HelWord HelSyscallOutput[2];

extern inline __attribute__ (( always_inline )) HelError helSyscall(int number,
		HelSyscallInput input, HelSyscallOutput output) {
	// note: the rcx and r11 registers are clobbered by syscall
	// so we do not use them to pass parameters
	// we also clobber rbx in kernel code
	register HelWord in1 asm("rdi") = number;
	register HelWord in2 asm("rsi") = input[0];
	register HelWord in3 asm("rdx") = input[1];
	register HelWord in4 asm("rax") = input[2];
	register HelWord in5 asm("r8") = input[3];
	register HelWord in6 asm("r9") = input[4];
	register HelWord in7 asm("r10") = input[5];
	register HelWord in8 asm("r12") = input[6];
	register HelWord in9 asm("r13") = input[7];
	register HelWord in10 asm("r14") = input[8];

	register HelWord out1 asm("rdi");
	register HelWord out2 asm("rsi");
	register HelWord out3 asm("rdx");
	register HelWord out4 asm("rax");
	register HelWord out5 asm("r8");
	register HelWord out6 asm("r9");
	register HelWord out7 asm("r10");
	register HelWord out8 asm("r12");
	register HelWord out9 asm("r13");
	register HelWord out10 asm("r14");
	
	asm volatile ( "syscall" : "=r" (out1), "=r" (out2), "=r" (out3), "=r" (out4), "=r" (out5),
				"=r" (out6), "=r" (out7), "=r" (out8), "=r" (out9), "=r" (out10)
			: "r" (in1), "r" (in2), "r" (in3), "r" (in4), "r" (in5),
				"r" (in6), "r" (in7), "r" (in8), "r" (in9), "r" (in10)
			: "rcx", "r11", "rbx", "memory" );

	output[0] = out2;
	output[1] = out3;
	return out1;
}

#define DEFINE_SYSCALL(name, ...) extern inline __attribute__ (( always_inline )) HelError hel ## name(__VA_ARGS__) { \
	HelSyscallInput in; HelSyscallOutput out;
#define IN(index, what) in[index] = (HelWord)what;
#define DO_SYSCALL(number) HelError error = helSyscall(kHelCall ## number, in, out);
#define OUT(index, type, what) *what = (type)out[index];
#define END_SYSCALL() return error; }

DEFINE_SYSCALL(Log, const char *string, size_t length)
	IN(0, string) IN(1, length)
	DO_SYSCALL(Log)
END_SYSCALL()

// note: this entry is intentionally different
extern inline void helPanic(const char *string, size_t length) {
	HelSyscallInput in; HelSyscallOutput out;
	IN(0, string) IN(1, length)
	helSyscall(kHelCallPanic, in, out);
	__builtin_unreachable();
}

DEFINE_SYSCALL(CreateUniverse, HelHandle *handle)
	DO_SYSCALL(CreateUniverse)
	OUT(0, HelHandle, handle)
END_SYSCALL()

DEFINE_SYSCALL(TransferDescriptor, HelHandle handle, HelHandle universe_handle,
		HelHandle *out_handle)
	IN(0, handle) IN(1, universe_handle)
	DO_SYSCALL(TransferDescriptor)
	OUT(0, HelHandle, out_handle)
END_SYSCALL()

DEFINE_SYSCALL(DescriptorInfo, HelHandle handle, struct HelDescriptorInfo *info)
	IN(0, handle) IN(1, info)
	DO_SYSCALL(DescriptorInfo)
END_SYSCALL()

DEFINE_SYSCALL(CloseDescriptor, HelHandle handle)
	IN(0, handle)
	DO_SYSCALL(CloseDescriptor)
END_SYSCALL()

DEFINE_SYSCALL(AllocateMemory, size_t size, uint32_t flags, HelHandle *handle)
	IN(0, size) IN(1, flags)
	DO_SYSCALL(AllocateMemory)
	OUT(0, HelHandle, handle)
END_SYSCALL()

DEFINE_SYSCALL(CreateManagedMemory, size_t size, uint32_t flags,
		HelHandle *backing_handle, HelHandle *frontal_handle)
	IN(0, size) IN(1, flags)
	DO_SYSCALL(CreateManagedMemory)
	OUT(0, HelHandle, backing_handle)
	OUT(1, HelHandle, frontal_handle)
END_SYSCALL()

DEFINE_SYSCALL(AccessPhysical, uintptr_t physical, size_t size, HelHandle *handle)
	IN(0, physical) IN(1, size)
	DO_SYSCALL(AccessPhysical)
	OUT(0, HelHandle, handle)
END_SYSCALL()

DEFINE_SYSCALL(CreateSpace, HelHandle *handle)
	DO_SYSCALL(CreateSpace)
	OUT(0, HelHandle, handle)
END_SYSCALL()

DEFINE_SYSCALL(ForkSpace, HelHandle handle, HelHandle *forked)
	IN(0, handle)
	DO_SYSCALL(ForkSpace)
	OUT(0, HelHandle, forked)
END_SYSCALL()

DEFINE_SYSCALL(MapMemory, HelHandle handle, HelHandle space,
		void *pointer, uintptr_t offset, size_t size, uint32_t flags, void **actual_pointer)
	IN(0, handle) IN(1, space) IN(2, pointer) IN(3, offset) IN(4, size) IN(5, flags)
	DO_SYSCALL(MapMemory)
	OUT(0, void *, actual_pointer)
END_SYSCALL()

DEFINE_SYSCALL(UnmapMemory, HelHandle space, void *pointer, size_t size)
	IN(0, space) IN(1, pointer) IN(2, size)
	DO_SYSCALL(UnmapMemory)
END_SYSCALL()

DEFINE_SYSCALL(PointerPhysical, void *pointer, uintptr_t *physical)
	IN(0, pointer)
	DO_SYSCALL(PointerPhysical)
	OUT(0, uintptr_t, physical)
END_SYSCALL()

DEFINE_SYSCALL(LoadForeign, HelHandle handle, uintptr_t address,
		size_t length, void *buffer)
	IN(0, handle) IN(1, address) IN(2, length) IN(3, buffer)
	DO_SYSCALL(LoadForeign)
END_SYSCALL()

DEFINE_SYSCALL(StoreForeign, HelHandle handle, uintptr_t address,
		size_t length, const void *buffer)
	IN(0, handle) IN(1, address) IN(2, length) IN(3, buffer)
	DO_SYSCALL(StoreForeign)
END_SYSCALL()

DEFINE_SYSCALL(MemoryInfo, HelHandle handle, size_t *size)
	IN(0, handle)
	DO_SYSCALL(MemoryInfo)
	OUT(0, size_t, size)
END_SYSCALL()

DEFINE_SYSCALL(SubmitProcessLoad, HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id)
	IN(0, handle) IN(1, hub_handle) IN(2, submit_function) IN(3, submit_object)
	DO_SYSCALL(SubmitProcessLoad)
	OUT(0, int64_t, async_id)
END_SYSCALL()

DEFINE_SYSCALL(CompleteLoad, HelHandle handle, uintptr_t offset, size_t length)
	IN(0, handle) IN(1, offset) IN(2, length)
	DO_SYSCALL(CompleteLoad)
END_SYSCALL()

DEFINE_SYSCALL(SubmitLockMemory, HelHandle handle, HelHandle hub_handle,
		uintptr_t offset, size_t size,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id)
	IN(0, handle) IN(1, hub_handle) IN(2, offset) IN(3, size)
		IN(4, submit_function) IN(5, submit_object)
	DO_SYSCALL(SubmitLockMemory)
	OUT(0, int64_t, async_id)
END_SYSCALL()

DEFINE_SYSCALL(Loadahead, HelHandle handle, uintptr_t offset, size_t length)
	IN(0, handle) IN(1, offset) IN(2, length)
	DO_SYSCALL(Loadahead)
END_SYSCALL()

DEFINE_SYSCALL(CreateThread, HelHandle universe, HelHandle address_space,
		HelAbi abi, void *ip, void *sp, uint32_t flags, HelHandle *handle)
	IN(0, universe) IN(1, address_space) IN(2, abi)
		IN(3, ip) IN(4, sp) IN(5, flags)
	DO_SYSCALL(CreateThread)
	OUT(0, HelHandle, handle)
END_SYSCALL()

DEFINE_SYSCALL(Yield)
	DO_SYSCALL(Yield)
END_SYSCALL()

DEFINE_SYSCALL(SubmitObserve, HelHandle handle,
		struct HelQueue *queue, uintptr_t context)
	IN(0, handle) IN(1, queue) IN(2, context)
	DO_SYSCALL(SubmitObserve)
END_SYSCALL()

DEFINE_SYSCALL(Resume, HelHandle handle)
	IN(0, handle)
	DO_SYSCALL(Resume)
END_SYSCALL()

DEFINE_SYSCALL(LoadRegisters, HelHandle handle, int set, void *image)
	IN(0, handle) IN(1, set) IN(2, image)
	DO_SYSCALL(LoadRegisters)
END_SYSCALL()

DEFINE_SYSCALL(StoreRegisters, HelHandle handle, int set, const void *image)
	IN(0, handle) IN(1, set) IN(2, image)
	DO_SYSCALL(StoreRegisters)
END_SYSCALL()

DEFINE_SYSCALL(ExitThisThread)
	DO_SYSCALL(ExitThisThread)
END_SYSCALL()

DEFINE_SYSCALL(WriteFsBase, void *pointer)
	IN(0, pointer)
	DO_SYSCALL(WriteFsBase)
END_SYSCALL()

DEFINE_SYSCALL(GetClock, uint64_t *counter)
	DO_SYSCALL(GetClock)
	OUT(0, uint64_t, counter)
END_SYSCALL()

DEFINE_SYSCALL(CreateEventHub, HelHandle *handle)
	DO_SYSCALL(CreateEventHub)
	OUT(0, HelHandle, handle)
END_SYSCALL()

DEFINE_SYSCALL(WaitForEvents, HelHandle handle,
		struct HelEvent *list, size_t max_items, HelNanotime max_time,
		size_t *num_items)
	IN(0, handle) IN(1, list) IN(2, max_items) IN(3, max_time)
	DO_SYSCALL(WaitForEvents)
	OUT(0, size_t, num_items)
END_SYSCALL()

DEFINE_SYSCALL(WaitForCertainEvent, HelHandle handle,
		int64_t async_id, struct HelEvent *event, HelNanotime max_time)
	IN(0, handle) IN(1, async_id) IN(2, event) IN(3, max_time)
	DO_SYSCALL(WaitForCertainEvent)
END_SYSCALL()


DEFINE_SYSCALL(CreateStream, HelHandle *lane1, HelHandle *lane2)
	DO_SYSCALL(CreateStream)
	OUT(0, HelHandle, lane1)
	OUT(1, HelHandle, lane2)
END_SYSCALL()

DEFINE_SYSCALL(SubmitAsync, HelHandle handle, const HelAction *actions,
		size_t count, struct HelQueue *queue, uint32_t flags)
	IN(0, handle) IN(1, actions) IN(2, count) IN(3, queue)
			IN(4, flags)
	DO_SYSCALL(SubmitAsync)
END_SYSCALL()


DEFINE_SYSCALL(FutexWait, int *pointer, int expected)
	IN(0, pointer); IN(1, expected);
	DO_SYSCALL(FutexWait)
END_SYSCALL()

DEFINE_SYSCALL(FutexWake, int *pointer)
	IN(0, pointer);
	DO_SYSCALL(FutexWake)
END_SYSCALL()


DEFINE_SYSCALL(AccessIrq, int number, HelHandle *handle)
	IN(0, number)
	DO_SYSCALL(AccessIrq)
	OUT(0, HelHandle, handle)
END_SYSCALL()

DEFINE_SYSCALL(SetupIrq, HelHandle handle, uint32_t flags)
	IN(0, handle) IN(1, handle)
	DO_SYSCALL(SetupIrq)
END_SYSCALL()

DEFINE_SYSCALL(AcknowledgeIrq, HelHandle handle)
	IN(0, handle)
	DO_SYSCALL(AcknowledgeIrq)
END_SYSCALL()

DEFINE_SYSCALL(SubmitWaitForIrq, HelHandle handle,
		struct HelQueue *queue, uintptr_t context)
	IN(0, handle) IN(1, queue) IN(2, context)
	DO_SYSCALL(SubmitWaitForIrq)
END_SYSCALL()

DEFINE_SYSCALL(SubscribeIrq, HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id)
	IN(0, handle) IN(1, hub_handle) IN(2, submit_function) IN(3, submit_object)
	DO_SYSCALL(SubscribeIrq)
	OUT(0, int64_t, async_id)
END_SYSCALL()

DEFINE_SYSCALL(AccessIo, uintptr_t *port_array, size_t num_ports, HelHandle *handle)
	IN(0, port_array) IN(1, num_ports)
	DO_SYSCALL(AccessIo)
	OUT(0, HelHandle, handle)
END_SYSCALL()

DEFINE_SYSCALL(EnableIo, HelHandle handle)
	IN(0, handle)
	DO_SYSCALL(EnableIo)
END_SYSCALL()

DEFINE_SYSCALL(EnableFullIo)
	DO_SYSCALL(EnableFullIo)
END_SYSCALL()

DEFINE_SYSCALL(ControlKernel, int subsystem, int interface,
		const void *input, void *output)
	IN(0, subsystem) IN(1, interface) IN(2, input) IN(3, output)
	DO_SYSCALL(ControlKernel)
END_SYSCALL()

#undef DEFINE_SYSCALL
#undef IN
#undef DO_SYSCALL
#undef OUT
#undef END_SYSCALL

#endif // HEL_SYSCALLS_H

