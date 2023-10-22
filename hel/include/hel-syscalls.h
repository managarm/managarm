
#ifndef HEL_SYSCALLS_H
#define HEL_SYSCALLS_H

#include <stddef.h>
#include <hel-types.h>
#include <hel-stubs.h>

extern inline __attribute__ (( always_inline )) HelError helLog(const char *string,
		size_t length) {
	return helSyscall2(kHelCallLog, (HelWord)string, length);
};

extern inline __attribute__ (( always_inline )) void helPanic(const char *string,
		size_t length) {
	helSyscall2(kHelCallPanic, (HelWord)string, length);
	__builtin_unreachable();
};

extern inline __attribute__ (( always_inline )) HelError helNop() {
	return helSyscall0(kHelCallNop);
}

extern inline __attribute__ (( always_inline )) HelError helSubmitAsyncNop(
		HelHandle queueHandle, uintptr_t context) {
	return helSyscall2(kHelCallSubmitAsyncNop,
			(HelWord)queueHandle, (HelWord)context);
}

extern inline __attribute__ (( always_inline )) HelError helCreateUniverse(HelHandle *handle) {
	HelWord handle_word;
	HelError error = helSyscall0_1(kHelCallCreateUniverse, &handle_word);
	*handle = (HelHandle)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helTransferDescriptor(HelHandle handle,
		HelHandle universe_handle, HelHandle *out_handle) {
	HelWord hel_out_handle;
	HelError error = helSyscall2_1(kHelCallTransferDescriptor, (HelWord)handle,
			(HelWord) universe_handle, &hel_out_handle);
	*out_handle = (HelHandle)hel_out_handle;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helDescriptorInfo(HelHandle handle,
		struct HelDescriptorInfo *info) {
	return helSyscall2(kHelCallDescriptorInfo, (HelWord)handle, (HelWord)info);
};

extern inline __attribute__ (( always_inline )) HelError helGetCredentials(HelHandle handle,
		uint32_t flags, char *credentials) {
	return helSyscall3(kHelCallGetCredentials, (HelWord)handle, (HelWord)flags,
			(HelWord)credentials);
};

extern inline __attribute__ (( always_inline )) HelError helCloseDescriptor(
		HelHandle universeHandle, HelHandle handle) {
	return helSyscall2(kHelCallCloseDescriptor, (HelWord)universeHandle, (HelWord)handle);
};

extern inline __attribute__ (( always_inline )) HelError helCreateQueue(
		struct HelQueueParameters *params, HelHandle *handle) {
	HelWord hel_handle;
	HelError error = helSyscall1_1(kHelCallCreateQueue, (HelWord)params, &hel_handle);
	*handle = (HelHandle)hel_handle;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helCancelAsync(HelHandle handle,
		uint64_t async_id) {
	return helSyscall2(kHelCallCancelAsync, (HelWord)handle, (HelWord)async_id);
};

extern inline __attribute__ (( always_inline )) HelError helAllocateMemory(size_t size,
		uint32_t flags, struct HelAllocRestrictions *restrictions, HelHandle *handle) {
	HelWord hel_handle;
	HelError error = helSyscall3_1(kHelCallAllocateMemory, (HelWord)size, (HelWord)flags,
			(HelWord)restrictions, &hel_handle);
	*handle = (HelHandle)hel_handle;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helResizeMemory(HelHandle handle,
		size_t size) {
	HelError error = helSyscall2(kHelCallResizeMemory, (HelWord)handle, (HelWord)size);
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helCreateManagedMemory(size_t size,
		uint32_t flags, HelHandle *backing_handle, HelHandle *frontal_handle) {
	HelWord back_handle;
	HelWord front_handle;
	HelError error = helSyscall2_2(kHelCallCreateManagedMemory, (HelWord)size, (HelWord)flags, 
			&back_handle, &front_handle);
	*backing_handle = (HelHandle)back_handle;
	*frontal_handle = (HelHandle)front_handle;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helCopyOnWrite(HelHandle memoryHandle,
		uintptr_t offset, size_t size, HelHandle *outHandle) {
	HelWord outWord;
	HelError error = helSyscall3_1(kHelCallCopyOnWrite, (HelWord)memoryHandle,
			(HelWord)offset, (HelWord)size, &outWord);
	*outHandle = (HelHandle)outWord;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helAccessPhysical(uintptr_t physical,
		size_t size, HelHandle *handle) {
	HelWord hel_handle;
	HelError error = helSyscall2_1(kHelCallAccessPhysical, (HelWord)physical, (HelWord)size, &hel_handle);
	*handle = (HelHandle)hel_handle;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helCreateIndirectMemory(
		size_t numSlots, HelHandle *handle) {
	HelWord helHandle;
	HelError error = helSyscall1_1(kHelCallCreateIndirectMemory,
			(HelWord)numSlots, &helHandle);
	*handle = (HelHandle)helHandle;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helAlterMemoryIndirection(
		HelHandle indirectHandle, size_t slotIndex,
		HelHandle memoryHandle, uintptr_t offset, size_t size) {
	HelError error = helSyscall5(kHelCallAlterMemoryIndirection,
			(HelWord)indirectHandle, (HelWord)slotIndex,
			(HelWord)memoryHandle, (HelWord)offset, (HelWord)size);
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helCreateSliceView(HelHandle bundle,
		uintptr_t offset, size_t size, uint32_t flags, HelHandle *handle) {
	HelWord hel_handle;
	HelError error = helSyscall4_1(kHelCallCreateSliceView, (HelWord)bundle,
			(HelWord)offset, (HelWord)size, (HelWord)flags, &hel_handle);
	*handle = (HelHandle)hel_handle;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helForkMemory(HelHandle handle,
		HelHandle *out_handle) {
	HelWord handle_word;
	HelError error = helSyscall1_1(kHelCallForkMemory, (HelWord)handle, &handle_word);
	*out_handle = (HelHandle)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helCreateSpace(HelHandle *handle) {
	HelWord handle_word;
	HelError error = helSyscall0_1(kHelCallCreateSpace, &handle_word);
	*handle = (HelHandle)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helCreateVirtualizedSpace(HelHandle *handle) {
	HelWord handle_word;
	HelError error = helSyscall0_1(kHelCallCreateVirtualizedSpace, &handle_word);
	*handle = (HelHandle)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helCreateVirtualizedCpu(HelHandle handle, HelHandle *out_handle) {
	HelWord handle_word;
	HelError error = helSyscall1_1(kHelCallCreateVirtualizedCpu, (HelWord)handle, &handle_word);
	*out_handle = (HelHandle)handle_word;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helRunVirtualizedCpu(HelHandle handle, struct HelVmexitReason *exitInfo) {
	HelError error = helSyscall2(kHelCallRunVirtualizedCpu, (HelWord)handle, (HelWord)exitInfo);
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helGetRandomBytes(
		void *buffer, size_t wantedSize, size_t *actualSize) {
	HelWord outActualSize;
	HelError error = helSyscall2_1(kHelCallGetRandomBytes,
			(HelWord)buffer, (HelWord)wantedSize, &outActualSize);
	*actualSize = (size_t)outActualSize;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helMapMemory(HelHandle handle,
		HelHandle space, void *pointer, uintptr_t offset, size_t size, uint32_t flags,
		void **actual_pointer) {
	HelWord out_ptr;
	HelError error = helSyscall6_1(kHelCallMapMemory, (HelWord)handle, (HelWord)space,
			(HelWord)pointer, (HelWord)offset, (HelWord)size, (HelWord)flags, &out_ptr);
	*actual_pointer = (void *)out_ptr;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helSubmitProtectMemory(HelHandle space,
		void *pointer, size_t size, uint32_t flags,
		HelHandle queue, uintptr_t context) {
	return helSyscall6(kHelCallSubmitProtectMemory, (HelWord)space,
			(HelWord)pointer, (HelWord)size, (HelWord)flags,
			(HelWord)queue, (HelWord)context);
};

extern inline __attribute__ (( always_inline )) HelError helUnmapMemory(HelHandle space,
		void *pointer, size_t size) {
	return helSyscall3(kHelCallUnmapMemory, (HelWord)space, (HelWord)pointer, (HelWord)size);
};

extern inline __attribute__ (( always_inline )) HelError helSubmitSynchronizeSpace(
		HelHandle space, void *pointer, size_t size,
		HelHandle queue, uintptr_t context) {
	return helSyscall5(kHelCallSubmitSynchronizeSpace,
			(HelWord)space, (HelWord)pointer, (HelWord)size,
			(HelWord)queue, (HelWord)context);
};

extern inline __attribute__ (( always_inline )) HelError helPointerPhysical(const void *pointer, 
		uintptr_t *physical) {
	HelWord handle_word;
	HelError error = helSyscall1_1(kHelCallPointerPhysical, (HelWord)pointer, &handle_word);
	*physical = (uintptr_t)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helSubmitReadMemory(HelHandle handle,
		uintptr_t address, size_t length, void *buffer,
		HelHandle queue, uintptr_t context) {
	return helSyscall6(kHelCallSubmitReadMemory, (HelWord)handle, (HelWord)address,
			(HelWord)length, (HelWord)buffer,
			(HelWord)queue, (HelWord)context);
};

extern inline __attribute__ (( always_inline )) HelError helSubmitWriteMemory(HelHandle handle,
		uintptr_t address, size_t length, const void *buffer,
		HelHandle queue, uintptr_t context) {
	return helSyscall6(kHelCallSubmitWriteMemory, (HelWord)handle, (HelWord)address,
			(HelWord)length, (HelWord)buffer,
			(HelWord)queue, (HelWord)context);
};

extern inline __attribute__ (( always_inline )) HelError helMemoryInfo(HelHandle handle, 
		size_t *size) {
	HelWord handle_word;
	HelError error = helSyscall1_1(kHelCallMemoryInfo, (HelWord)handle, &handle_word);
	*size = (size_t)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helSubmitManageMemory(HelHandle handle,
		HelHandle queue, uintptr_t context) {
	return helSyscall3(kHelCallSubmitManageMemory, (HelWord)handle, (HelWord)queue, (HelWord)context);
};

extern inline __attribute__ (( always_inline )) HelError helUpdateMemory(HelHandle handle,
		int type, uintptr_t offset, size_t length) {
	return helSyscall4(kHelCallUpdateMemory, (HelWord)handle, (HelWord)type,
			(HelWord)offset, (HelWord)length);
};

extern inline __attribute__ (( always_inline )) HelError helSubmitLockMemoryView(HelHandle handle,
		uintptr_t offset, size_t size, HelHandle queue, uintptr_t context) {
	return helSyscall5(kHelCallSubmitLockMemoryView, (HelWord)handle, (HelWord)offset,
			(HelWord)size, (HelWord)queue, (HelWord)context);
};

extern inline __attribute__ (( always_inline )) HelError helLoadahead(HelHandle handle,
		uintptr_t offset, size_t length) {
	return helSyscall3(kHelCallLoadahead, (HelWord)handle, (HelWord)offset, (HelWord)length);
};

extern inline __attribute__ (( always_inline )) HelError helCreateThread(HelHandle universe,
		HelHandle address_space, HelAbi abi, void *ip, void *sp, uint32_t flags,
		HelHandle *handle) {
	HelWord out_handle;
	HelError error = helSyscall6_1(kHelCallCreateThread, (HelWord)universe, (HelWord)address_space,
			(HelWord)abi, (HelWord)ip, (HelWord)sp, (HelWord)flags, &out_handle);
	*handle = (HelHandle)out_handle;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helQueryThreadStats(HelHandle handle,
		struct HelThreadStats *stats) {
	return helSyscall2(kHelCallQueryThreadStats, (HelWord)handle, (HelWord)stats);
};

extern inline __attribute__ (( always_inline )) HelError helYield() {
	return helSyscall0(kHelCallYield);
};

extern inline __attribute__ (( always_inline )) HelError helSetPriority(HelHandle handle,
		int priority) {
	return helSyscall2(kHelCallSetPriority, (HelWord)handle, (HelWord)priority);
};

extern inline __attribute__ (( always_inline )) HelError helSubmitObserve(HelHandle handle,
		uint64_t in_seq, HelHandle queue, uintptr_t context) {
	return helSyscall4(kHelCallSubmitObserve, (HelWord)handle, (HelWord)in_seq,
			(HelWord)queue, (HelWord)context);
};

extern inline __attribute__ (( always_inline )) HelError helKillThread(HelHandle handle) {
	return helSyscall1(kHelCallKillThread, (HelWord)handle);
};

extern inline __attribute__ (( always_inline )) HelError helInterruptThread(HelHandle handle) {
	return helSyscall1(kHelCallInterruptThread, (HelWord)handle);
};

extern inline __attribute__ (( always_inline )) HelError helResume(HelHandle handle) {
	return helSyscall1(kHelCallResume, (HelWord)handle);
};

extern inline __attribute__ (( always_inline )) HelError helLoadRegisters(HelHandle handle,
		int set, void *image) {
	return helSyscall3(kHelCallLoadRegisters, (HelWord)handle, (HelWord)set, (HelWord)image);
};

extern inline __attribute__ (( always_inline )) HelError helStoreRegisters(HelHandle handle,
		int set, const void *image) {
	return helSyscall3(kHelCallStoreRegisters, (HelWord)handle, (HelWord)set, (HelWord)image);
};

extern inline __attribute__ (( always_inline )) HelError helWriteFsBase(void *pointer) {
	return helSyscall1(kHelCallWriteFsBase, (HelWord)pointer);
};

extern inline __attribute__ (( always_inline )) HelError helReadFsBase(void **pointer) {
	return helSyscall1(kHelCallReadFsBase, (HelWord)pointer);
};

extern inline __attribute__ (( always_inline )) HelError helWriteGsBase(void *pointer) {
	return helSyscall1(kHelCallWriteGsBase, (HelWord)pointer);
};

extern inline __attribute__ (( always_inline )) HelError helReadGsBase(void **pointer) {
	return helSyscall1(kHelCallReadGsBase, (HelWord)pointer);
};

extern inline __attribute__ (( always_inline )) HelError helGetClock(uint64_t *counter) {
	HelWord handle_word;
	HelError error = helSyscall0_1(kHelCallGetClock, &handle_word);
	*counter = (uint64_t)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helSubmitAwaitClock(uint64_t counter,
		HelHandle queue, uintptr_t context, uint64_t *async_id) {
	HelWord async_word;
	HelError error = helSyscall3_1(kHelCallSubmitAwaitClock, (HelWord)counter, (HelWord)queue,
			(HelWord)context, &async_word);
	*async_id = (uint64_t)async_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helCreateStream(HelHandle *lane1,
		HelHandle *lane2) {
	HelWord out_lane1;
	HelWord out_lane2;
	HelError error = helSyscall0_2(kHelCallCreateStream, &out_lane1, &out_lane2);
	*lane1 = (HelHandle)out_lane1;
	*lane2 = (HelHandle)out_lane2;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helSubmitAsync(HelHandle handle,
		const struct HelAction *actions, size_t count, HelHandle queue, uintptr_t context,
		uint32_t flags) {
	return helSyscall6(kHelCallSubmitAsync, (HelWord)handle, (HelWord)actions, (HelWord)count,
			(HelWord)queue, (HelWord)context, (HelWord)flags);
};

extern inline __attribute__ (( always_inline )) HelError helShutdownLane(HelHandle handle) {
	return helSyscall1(kHelCallShutdownLane, (HelWord)handle);
};

extern inline __attribute__ (( always_inline )) HelError helFutexWait(int *pointer,
		int expected, int64_t deadline) {
	return helSyscall3(kHelCallFutexWait, (HelWord)pointer, (HelWord)expected,
			(HelWord)deadline);
};

extern inline __attribute__ (( always_inline )) HelError helFutexWake(int *pointer) {
	return helSyscall1(kHelCallFutexWake, (HelWord)pointer);
};

extern inline __attribute__ (( always_inline )) HelError helCreateOneshotEvent(HelHandle *handle) {
	HelWord handle_word;
	HelError error = helSyscall0_1(kHelCallCreateOneshotEvent, &handle_word);
	*handle = (HelHandle)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helCreateBitsetEvent(HelHandle *handle) {
	HelWord handle_word;
	HelError error = helSyscall0_1(kHelCallCreateBitsetEvent, &handle_word);
	*handle = (HelHandle)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helRaiseEvent(HelHandle handle) {
	return helSyscall1(kHelCallRaiseEvent, (HelWord)handle);
};

extern inline __attribute__ (( always_inline )) HelError helAccessIrq(int number, 
		HelHandle *handle) {
	HelWord handle_word;
	HelError error = helSyscall1_1(kHelCallAccessIrq, number, &handle_word);
	*handle = (HelHandle)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helAcknowledgeIrq(HelHandle handle,
		uint32_t flags, uint64_t sequence) {
	return helSyscall3(kHelCallAcknowledgeIrq, (HelWord)handle, (HelWord)flags,
			(HelWord)sequence);
};

extern inline __attribute__ (( always_inline )) HelError helSubmitAwaitEvent(HelHandle handle,
		uint64_t sequence, HelHandle queue, uintptr_t context) {
	return helSyscall4(kHelCallSubmitAwaitEvent, (HelWord)handle, (HelWord)sequence,
			(HelWord)queue, (HelWord)context);
};

extern inline __attribute__ (( always_inline )) HelError helAutomateIrq(HelHandle handle,
		uint32_t flags, HelHandle kernlet) {
	return helSyscall3(kHelCallAutomateIrq, (HelWord)handle, (HelWord)flags,
			(HelWord)kernlet);
};

extern inline __attribute__ (( always_inline )) HelError helAccessIo(uintptr_t *port_array,
		size_t num_ports, HelHandle *handle) {
	HelWord out_handle;
	HelError error = helSyscall2_1(kHelCallAccessIo, (HelWord)port_array,
			(HelWord) num_ports, &out_handle);
	*handle = (HelHandle)out_handle;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helEnableIo(HelHandle handle) {
	return helSyscall1(kHelCallEnableIo, (HelWord)handle);
};

extern inline __attribute__ (( always_inline )) HelError helEnableFullIo() {
	return helSyscall0(kHelCallEnableFullIo);
};

extern inline __attribute__ (( always_inline )) HelError helBindKernlet(HelHandle handle,
		const union HelKernletData *data, size_t num_data, HelHandle *bound_handle) {
	HelWord handle_word;
	HelError error = helSyscall3_1(kHelCallBindKernlet, (HelWord)handle,
			(HelWord)data, (HelWord)num_data, &handle_word);
	*bound_handle = (HelHandle)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helGetAffinity(HelHandle handle,
		uint8_t *mask, size_t size, size_t *actualSize) {
	return helSyscall4(kHelCallGetAffinity, (HelWord)handle,
			(HelWord)mask, (HelWord)size, (HelWord)actualSize);
};

extern inline __attribute__ (( always_inline )) HelError helSetAffinity(HelHandle handle,
		uint8_t *mask, size_t size) {
	return helSyscall3(kHelCallSetAffinity, (HelWord)handle,
			(HelWord)mask, (HelWord)size);
};

extern inline __attribute__ (( always_inline )) HelError helQueryRegisterInfo(int set,
		struct HelRegisterInfo *info) {
	return helSyscall2(kHelCallQueryRegisterInfo, (HelWord)set, (HelWord)info);
};

extern inline __attribute__ (( always_inline )) HelError helGetCpuInformation(uint32_t *cores) {
	HelWord cores_word;
	HelError error = helSyscall0_1(kHelCallGetCpuInformation, &cores_word);
	*cores = (uint32_t)cores_word;
	return error;
}

extern inline __attribute__ (( always_inline )) HelError helGetSchedulerInformation(size_t buffer_size, HelSchedulerStats* cpu_buffer, HelSchedulerStats* global) {
	return helSyscall3(kHelCallGetSchedulerInformation, (HelWord)buffer_size, (HelWord)cpu_buffer, (HelWord)global);
}

#endif // HEL_SYSCALLS_H

