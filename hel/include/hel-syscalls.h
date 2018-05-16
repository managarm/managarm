
#ifndef HEL_SYSCALLS_H
#define HEL_SYSCALLS_H

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

extern inline __attribute__ (( always_inline )) HelError helCloseDescriptor(HelHandle handle) {
	return helSyscall1(kHelCallCloseDescriptor, (HelWord)handle);
};

extern inline __attribute__ (( always_inline )) HelError helCreateQueue(HelQueue *head,
		uint32_t flags, HelHandle *handle) {
	HelWord hel_handle;
	HelError error = helSyscall2_1(kHelCallCreateQueue, (HelWord)head, (HelWord)flags, &hel_handle);
	*handle = (HelHandle)hel_handle;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helSetupChunk(HelHandle queue,
		int index, HelChunk *chunk, uint32_t flags) {
	return helSyscall4(kHelCallSetupChunk, (HelWord)queue, (HelWord)index,
			(HelWord)chunk, (HelWord)flags);
};

extern inline __attribute__ (( always_inline )) HelError helAllocateMemory(size_t size,
		uint32_t flags, HelHandle *handle) {
	HelWord hel_handle;
	HelError error = helSyscall2_1(kHelCallAllocateMemory, (HelWord)size, (HelWord)flags, &hel_handle);
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

extern inline __attribute__ (( always_inline )) HelError helAccessPhysical(uintptr_t physical,
		size_t size, HelHandle *handle) {
	HelWord hel_handle;
	HelError error = helSyscall2_1(kHelCallAccessPhysical, (HelWord)physical, (HelWord)size, &hel_handle);
	*handle = (HelHandle)hel_handle;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helCreateSliceView(HelHandle bundle,
		uintptr_t offset, size_t size, uint32_t flags, HelHandle *handle) {
	HelWord hel_handle;
	HelError error = helSyscall4_1(kHelCallCreateSliceView, (HelWord)bundle,
			(HelWord)offset, (HelWord)size, (HelWord)flags, &hel_handle);
	*handle = (HelHandle)hel_handle;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helCreateSpace(HelHandle *handle) {
	HelWord handle_word;
	HelError error = helSyscall0_1(kHelCallCreateSpace, &handle_word);
	*handle = (HelHandle)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helForkSpace(HelHandle handle, 
		HelHandle *out_handle) {
	HelWord handle_word;
	HelError error = helSyscall1_1(kHelCallForkSpace, (HelWord)handle, &handle_word);
	*out_handle = (HelHandle)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helMapMemory(HelHandle handle,
		HelHandle space, void *pointer, uintptr_t offset, size_t size, uint32_t flags,
		void **actual_pointer) {
	HelWord out_ptr;
	HelError error = helSyscall6_1(kHelCallMapMemory, (HelWord)handle, (HelWord)space,
			(HelWord)pointer, (HelWord)offset, (HelWord)size, (HelWord)flags, &out_ptr);
	*actual_pointer = (void *)out_ptr;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helUnmapMemory(HelHandle space,
		void *pointer, size_t size) {
	return helSyscall3(kHelCallUnmapMemory, (HelWord)space, (HelWord)pointer, (HelWord)size);
};

extern inline __attribute__ (( always_inline )) HelError helPointerPhysical(void *pointer, 
		uintptr_t *physical) {
	HelWord handle_word;
	HelError error = helSyscall1_1(kHelCallPointerPhysical, (HelWord)pointer, &handle_word);
	*physical = (uintptr_t)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helLoadForeign(HelHandle handle,
		uintptr_t address, size_t length, void *buffer) {
	return helSyscall4(kHelCallLoadForeign, (HelWord)handle, (HelWord)address,
			(HelWord)length, (HelWord)buffer);
};

extern inline __attribute__ (( always_inline )) HelError helStoreForeign(HelHandle handle,
		uintptr_t address, size_t length, const void *buffer) {
	return helSyscall4(kHelCallStoreForeign, (HelWord)handle, (HelWord)address,
			(HelWord)length, (HelWord)buffer);
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

extern inline __attribute__ (( always_inline )) HelError helCompleteLoad(HelHandle handle,
		uintptr_t offset, size_t length) {
	return helSyscall3(kHelCallCompleteLoad, (HelWord)handle, (HelWord)offset, (HelWord)length);
};

extern inline __attribute__ (( always_inline )) HelError helSubmitLockMemory(HelHandle handle,
		uintptr_t offset, size_t size, HelHandle queue, uintptr_t context) {
	return helSyscall5(kHelCallSubmitLockMemory, (HelWord)handle, (HelWord)offset,
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

extern inline __attribute__ (( always_inline )) HelError helYield() {
	return helSyscall0(kHelCallYield);
};

extern inline __attribute__ (( always_inline )) HelError helSetPriority(HelHandle handle,
		int priority) {
	return helSyscall2(kHelCallSetPriority, (HelWord)handle, (HelWord)priority);
};

extern inline __attribute__ (( always_inline )) HelError helSubmitObserve(HelHandle handle,
		HelHandle queue, uintptr_t context) {
	return helSyscall3(kHelCallSubmitObserve, (HelWord)handle, (HelWord)queue, (HelWord)context);
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

extern inline __attribute__ (( always_inline )) HelError helGetClock(uint64_t *counter) {
	HelWord handle_word;
	HelError error = helSyscall0_1(kHelCallGetClock, &handle_word);
	*counter = (uint64_t)handle_word;
	return error;
};

extern inline __attribute__ (( always_inline )) HelError helSubmitAwaitClock(uint64_t counter,
		HelHandle queue, uintptr_t context) {
	return helSyscall3(kHelCallSubmitAwaitClock, (HelWord)counter, (HelWord)queue, (HelWord)context);
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
		const HelAction *actions, size_t count, HelHandle queue, uintptr_t context,
		uint32_t flags) {
	return helSyscall6(kHelCallSubmitAsync, (HelWord)handle, (HelWord)actions, (HelWord)count,
			(HelWord)queue, (HelWord)context, (HelWord)flags);
};

extern inline __attribute__ (( always_inline )) HelError helFutexWait(int *pointer,
		int expected) {
	return helSyscall2(kHelCallFutexWait, (HelWord)pointer, (HelWord)expected);
};

extern inline __attribute__ (( always_inline )) HelError helFutexWake(int *pointer) {
	return helSyscall1(kHelCallFutexWake, (HelWord)pointer);
};

extern inline __attribute__ (( always_inline )) HelError helAccessIrq(int number, 
		HelHandle *handle) {
	HelWord handle_word;
	HelError error = helSyscall2_1(kHelCallAccessIrq, number, (HelWord)handle, &handle_word);
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

#endif // HEL_SYSCALLS_H

