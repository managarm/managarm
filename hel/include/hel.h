
//! @file hel.h

#ifndef HEL_H
#define HEL_H

#include <stddef.h>
#include <string.h>
#include <stdint.h>

#ifdef __cplusplus
#define HEL_C_LINKAGE extern "C"
#else
#define HEL_C_LINKAGE
#endif

enum {
	// largest system call number plus 1
	kHelNumCalls = 102,

	kHelCallLog = 1,
	kHelCallPanic = 10,

	kHelCallCreateUniverse = 62,
	kHelCallTransferDescriptor = 66,
	kHelCallDescriptorInfo = 32,
	kHelCallGetCredentials = 84,
	kHelCallCloseDescriptor = 21,

	kHelCallCreateQueue = 89,
	kHelCallSetupChunk = 90,
	kHelCallCancelAsync = 92,

	kHelCallAllocateMemory = 51,
	kHelCallResizeMemory = 83,
	kHelCallCreateManagedMemory = 64,
	kHelCallCopyOnWrite = 39,
	kHelCallAccessPhysical = 30,
	kHelCallCreateSliceView = 88,
	kHelCallForkMemory = 40,
	kHelCallCreateSpace = 27,
	kHelCallCreateIndirectMemory = 45,
	kHelCallAlterMemoryIndirection = 52,
	kHelCallMapMemory = 44,
	kHelCallSubmitProtectMemory = 99,
	kHelCallSubmitSynchronizeSpace = 53,
	kHelCallUnmapMemory = 36,
	kHelCallPointerPhysical = 43,
	kHelCallSubmitReadMemory = 77,
	kHelCallSubmitWriteMemory = 78,
	kHelCallMemoryInfo = 26,
	kHelCallSubmitManageMemory = 46,
	kHelCallUpdateMemory = 47,
	kHelCallSubmitLockMemoryView = 48,
	kHelCallLoadahead = 49,
	kHelCallCreateVirtualizedSpace = 50,

	kHelCallCreateThread = 67,
	kHelCallQueryThreadStats = 95,
	kHelCallSetPriority = 85,
	kHelCallYield = 34,
	kHelCallSubmitObserve = 74,
	kHelCallKillThread = 87,
	kHelCallInterruptThread = 86,
	kHelCallResume = 61,
	kHelCallLoadRegisters = 75,
	kHelCallStoreRegisters = 76,
	kHelCallWriteFsBase = 41,
	kHelCallGetClock = 42,
	kHelCallSubmitAwaitClock = 80,
	kHelCallCreateVirtualizedCpu = 37,
	kHelCallRunVirtualizedCpu = 38,
	kHelCallGetRandomBytes = 101,

	kHelCallCreateStream = 68,
	kHelCallSubmitAsync = 79,
	kHelCallShutdownLane = 91,

	kHelCallFutexWait = 73,
	kHelCallFutexWake = 71,

	kHelCallCreateOneshotEvent = 96,
	kHelCallCreateBitsetEvent = 97,
	kHelCallRaiseEvent = 98,
	kHelCallAccessIrq = 14,
	kHelCallAcknowledgeIrq = 81,
	kHelCallSubmitAwaitEvent = 82,
	kHelCallAutomateIrq = 94,

	kHelCallAccessIo = 11,
	kHelCallEnableIo = 12,
	kHelCallEnableFullIo = 35,

	kHelCallBindKernlet = 93,

	kHelCallSetAffinity = 100,

	kHelCallSuper = 0x80000000
};

enum {
	kHelErrNone = 0,
	kHelErrIllegalSyscall = 5,
	kHelErrIllegalArgs = 7,
	kHelErrIllegalState = 15,
	kHelErrUnsupportedOperation = 18,
	kHelErrOutOfBounds = 19,
	kHelErrQueueTooSmall = 14,
	kHelErrCancelled = 12,
	kHelErrNoDescriptor = 4,
	kHelErrBadDescriptor = 2,
	kHelErrThreadTerminated = 11,
	kHelErrTransmissionMismatch = 13,
	kHelErrLaneShutdown = 8,
	kHelErrEndOfLane = 9,
	kHelErrBufferTooSmall = 1,
	kHelErrFault = 10,
	kHelErrNoHardwareSupport = 16,
	kHelErrNoMemory = 17,
};

struct HelX86SegmentRegister {
	uint64_t base;
	uint32_t limit;
	uint16_t selector;
	uint32_t ar_bytes;
	uint8_t access_right;
};

struct HelX86DescriptorTable {
	uint64_t base;
	uint16_t limit;
};

struct HelX86VirtualizationRegs {
	uint64_t rax;
	uint64_t rbx;
	uint64_t rcx;
	uint64_t rdx;
	uint64_t rsi;
	uint64_t rdi;
	uint64_t rbp;
	uint64_t r8;
	uint64_t r9;
	uint64_t r10;
	uint64_t r11;
	uint64_t r12;
	uint64_t r13;
	uint64_t r14;
	uint64_t r15;

	uint64_t rsp;
	uint64_t rip;
	uint64_t rflags;

	struct HelX86SegmentRegister cs, ds, es, fs, gs, ss;
	struct HelX86SegmentRegister tr, ldt;
	struct HelX86DescriptorTable gdt, idt;

	uint64_t cr0, cr2, cr3, cr4, cr8;
	uint64_t efer;
	uint64_t apic_base;
};

//! Integer type that represents an error or success value.
typedef int HelError;

typedef int HelAbi;

//! Integer handle that represents a kernel resource.
typedef int64_t HelHandle;

typedef int64_t HelNanotime;

enum {
	kHelNullHandle = 0,
	kHelThisUniverse = -1,
	kHelThisThread = -2
};

enum {
	kHelWaitInfinite = -1
};

enum {
	kHelAbiSystemV = 1
};

enum {
	kHelActionOffer = 5,
	kHelActionAccept = 6,
	kHelActionImbueCredentials = 8,
	kHelActionExtractCredentials = 9,
	kHelActionSendFromBuffer = 1,
	kHelActionSendFromBufferSg = 10,
	kHelActionRecvInline = 7,
	kHelActionRecvToBuffer = 3,
	kHelActionPushDescriptor = 2,
	kHelActionPullDescriptor = 4
};

enum {
	kHelItemChain = 1,
	kHelItemAncillary = 2,
};

struct HelSgItem {
	void *buffer;
	size_t length;
};

struct HelAction {
	int type;
	uint32_t flags;
	// TODO: the following fields could be put into unions
	void *buffer;
	size_t length;
	HelHandle handle;
};

enum {
	kHelDescMemory = 1,
	kHelDescAddressSpace = 2,
	kHelDescThread = 3,
	kHelDescEndpoint = 5,
	kHelDescIrq = 9,
	kHelDescIo = 10,
};

struct HelDescriptorInfo {
	int type;
};

enum HelAllocFlags {
	kHelAllocContinuous = 4,
	kHelAllocOnDemand = 1,
	kHelAllocBacked = 2
};

struct HelAllocRestrictions {
	int addressBits;
};

enum HelManageRequests {
	kHelManageInitialize = 1,
	kHelManageWriteback = 2
};

enum HelMapFlags {
	// Additional flags that may be set.
	kHelMapProtRead = 256,
	kHelMapProtWrite = 512,
	kHelMapProtExecute = 1024,
	kHelMapDontRequireBacking = 128
};

enum HelThreadFlags {
	kHelThreadStopped = 1
};

enum HelObservation {
	kHelObserveNull = 0,
	kHelObserveInterrupt = 4,
	kHelObservePanic = 3,
	kHelObserveBreakpoint = 1,
	kHelObservePageFault = 2,
	kHelObserveGeneralFault = 5,
	kHelObserveIllegalInstruction = 6,
	kHelObserveSuperCall = 0x80000000
};

enum HelRegisterSets {
	kHelRegsProgram = 1,
	kHelRegsGeneral = 2,
	kHelRegsThread = 3,
	kHelRegsDebug = 4,
	kHelRegsVirtualization = 5
};

#if defined(__x86_64__)
enum HelRegisterIndex {
	kHelRegRax = 0,
	kHelRegRbx = 1,
	kHelRegRcx = 2,
	kHelRegRdx = 3,
	kHelRegRdi = 4,
	kHelRegRsi = 5,
	kHelRegR8 = 6,
	kHelRegR9 = 7,
	kHelRegR10 = 8,
	kHelRegR11 = 9,
	kHelRegR12 = 10,
	kHelRegR13 = 11,
	kHelRegR14 = 12,
	kHelRegR15 = 13,
	kHelRegRbp = 14,

	kHelNumGprs = 15,

	kHelRegIp = 0,
	kHelRegSp = 1
};

enum HelSyscallArgs {
	kHelRegNumber = kHelRegRdi,
	kHelRegError = kHelRegRdi,

	kHelRegArg0 = kHelRegRsi,
	kHelRegArg1 = kHelRegRdx,
	kHelRegArg2 = kHelRegRax,
	kHelRegArg3 = kHelRegR8,
	kHelRegArg4 = kHelRegR9,
	kHelRegArg5 = kHelRegR10,
	kHelRegArg6 = kHelRegR12,
	kHelRegArg7 = kHelRegR13,
	kHelRegArg8 = kHelRegR14,

	kHelRegOut0 = kHelRegRsi,
	kHelRegOut1 = kHelRegRdx
};

#elif defined(__aarch64__)
enum HelRegisterIndex {
	kHelRegX0 = 0,
	kHelRegX1,
	kHelRegX2,
	kHelRegX3,
	kHelRegX4,
	kHelRegX5,
	kHelRegX6,
	kHelRegX7,
	kHelRegX8,
	kHelRegX9,
	kHelRegX10,
	kHelRegX11,
	kHelRegX12,
	kHelRegX13,
	kHelRegX14,
	kHelRegX15,
	kHelRegX16,
	kHelRegX17,
	kHelRegX18,
	kHelRegX19,
	kHelRegX20,
	kHelRegX21,
	kHelRegX22,
	kHelRegX23,
	kHelRegX24,
	kHelRegX25,
	kHelRegX26,
	kHelRegX27,
	kHelRegX28,
	kHelRegX29,
	kHelRegX30,

	kHelNumGprs = 31,

	kHelRegIp = 0,
	kHelRegSp = 1
};

enum HelSyscallArgs {
	kHelRegNumber = kHelRegX0,
	kHelRegError = kHelRegX0,

	kHelRegArg0 = kHelRegX1,
	kHelRegArg1 = kHelRegX2,
	kHelRegArg2 = kHelRegX3,
	kHelRegArg3 = kHelRegX4,
	kHelRegArg4 = kHelRegX5,
	kHelRegArg5 = kHelRegX6,
	kHelRegArg6 = kHelRegX7,
	kHelRegArg7 = kHelRegX8,
	kHelRegArg8 = kHelRegX9,

	kHelRegOut0 = kHelRegX1,
	kHelRegOut1 = kHelRegX2
};
#endif

enum HelMessageFlags {
	kHelRequest = 1,
	kHelResponse = 2
};

//! Mask to extract the current queue head.
static const int kHelHeadMask = 0xFFFFFF;

//! Can be set by the kernel to request a FutexWake on update
static const int kHelHeadWaiters = (1 << 24);

//! In-memory kernel/user-space queue.
struct HelQueue {
	//! Futex for kernel/user-space head synchronization.
	int headFutex;

	//! Ensures that the buffer is 8-byte aligned.
	char padding[4];

	//! The actual queue.
	int indexQueue[];
};

//! Mask to extract the number of valid bytes in the chunk.
static const int kHelProgressMask = 0xFFFFFF;

//! Can be set by userspace to request a FutexWake on update.
static const int kHelProgressWaiters = (1 << 24);

//! Set by the kernel once it retires the chunk.
static const int kHelProgressDone = (1 << 25);

struct HelChunk {
	//! Futex for kernel/user-space progress synchronization.
	int progressFutex;

	//! Ensures that the buffer is 8-byte aligned.
	char padding[4];

	//! Actual contents of the chunk.
	char buffer[];
};

//! A single element of a HelQueue.
struct HelElement {
	//! Length of the element in bytes.
	unsigned int length;
	unsigned int reserved;
	//! User-defined value.
	void *context;
};

struct HelSimpleResult {
	HelError error;
	int reserved;
};

struct HelCredentialsResult {
	HelError error;
	int reserved;
	char credentials[16];
};

struct HelManageResult {
	HelError error;
	int type;
	uintptr_t offset;
	size_t length;
};

struct HelObserveResult {
	HelError error;
	unsigned int observation;
	uint64_t sequence;
};

struct HelInlineResult {
	HelError error;
	int reserved;
	size_t length;
	char data[];
};

struct HelInlineResultNoFlex {
	HelError error;
	int reserved;
	size_t length;
};

struct HelLengthResult {
	HelError error;
	int reserved;
	size_t length;
};

struct HelHandleResult {
	HelError error;
	int reserved;
	HelHandle handle;
};

struct HelEventResult {
	HelError error;
	uint32_t bitset;
	uint64_t sequence;
};

enum HelIrqFlags {
	kHelIrqExclusive = 1,
	kHelIrqManualAcknowledge = 2
};

enum HelAckFlags {
	kHelAckAcknowledge = 2,
	kHelAckNack = 3,
	kHelAckKick = 1
};

union HelKernletData {
	HelHandle handle;
};

struct HelThreadStats {
	uint64_t userTime;
};

enum {
  khelVmexitHlt = 0,
  khelVmexitTranslationFault = 1,
  khelVmexitError = -1,
};

struct HelVmexitReason {
	uint32_t exitReason;
	size_t address;
	size_t flags;
};

//! @name Logging
//! @{

//! Writes a text message (e.g., a line of text) to the kernel's log.
//! @param[in] string
//!    	Text to be written.
//! @param[in] length
//! 	Size of the text in bytes.
HEL_C_LINKAGE HelError helLog(const char *string, size_t length);

//! Kills the current thread and writes an error message to the kernel's log.
//! @param[in] string
//!    	Text to be written.
//! @param[in] length
//! 	Size of the text in bytes.
HEL_C_LINKAGE void helPanic(const char *string, size_t length)
		__attribute__ (( noreturn ));

//! @}
//! @name Management of Descriptors and Universes
//! @{

//! Creates a new universe descriptor.
//! @param[out] handle
//!    	Handle to the new universe descriptor.
HEL_C_LINKAGE HelError helCreateUniverse(HelHandle *handle);

//! Copies descriptors from the current universe to another universe.
//! @param[in] handle
//!    	Handle the descriptor to transfer.
//! @param[in] universeHandle
//!    	Handle to the destination universe.
//! @param[out] outHandle
//!    	Handle to the copied descriptor (valid in the universe specified by @p universeHandle).
HEL_C_LINKAGE HelError helTransferDescriptor(HelHandle handle, HelHandle universeHandle,
		HelHandle *outHandle);

HEL_C_LINKAGE HelError helDescriptorInfo(HelHandle handle, struct HelDescriptorInfo *info);

//! Returns the credentials associated with a given descriptor.
//! @param[in] handle
//!    	Handle to the descriptor.
//!    @param[out] credentials
//!    	Credentials, i.e., a 16-byte binary string.
HEL_C_LINKAGE HelError helGetCredentials(HelHandle handle, uint32_t flags,
		char *credentials);

//! Closes a descriptor.
//! @param[in] universeHandle
//!    	Handle to the universe containing @p handle.
//! @param[in] handle
//!    	Handle to be closed.
HEL_C_LINKAGE HelError helCloseDescriptor(HelHandle universeHandle, HelHandle handle);

//! @}
//! @name Management of IPC Queues
//! @{

//! size_shift:    Size of the indexQueue array.
//! element_limit: Maximum size of a single element in bytes.
//!                Does not include the per-element HelElement header.
HEL_C_LINKAGE HelError helCreateQueue(struct HelQueue *head, uint32_t flags,
		unsigned int size_shift, size_t element_limit, HelHandle *handle);

HEL_C_LINKAGE HelError helSetupChunk(HelHandle queue, int index, struct HelChunk *chunk, uint32_t flags);

//! Cancels an ongoing asynchronous operation.
//! @param[in] queueHandle
//!    	Handle to the queue that the operation was submitted to.
//! @param[in] asyncId
//!    	ID identifying the operation.
HEL_C_LINKAGE HelError helCancelAsync(HelHandle queueHandle, uint64_t asyncId);

//! @}
//! @name Memory Management
//! @{

//! Creates a memory object consisting of unmanaged RAM.
//! @param[in] size
//!    	Size of the memory object in bytes.
//!    	Must be aligned to the system's page size.
//! @param[in] restrictions
//!    	Specifies restrictions for the kernel's memory allocator.
//!    	May be @p NULL if there are no restrictions.
//! @param[out] handle
//!    	Handle to the new memory object.
HEL_C_LINKAGE HelError helAllocateMemory(size_t size, uint32_t flags,
		struct HelAllocRestrictions *restrictions, HelHandle *handle);

//! Resizes a memory object.
//! @param[in] handle
//!    	Handle to the memory object.
//!    	Must be aligned to the system's page size.
//! @param[in] newSize
//!    	New size in bytes.
HEL_C_LINKAGE HelError helResizeMemory(HelHandle handle, size_t newSize);

//! Creates a memory object that is managed by userspace.
//!
//!    The @p backingHandle is used to manage the memory object, while
//! the @p frontalHandle provides a view on the memory object for consumers.
//! @param[in] size
//!    	Size of the memory object in bytes.
//!    	Must be aligned to the system's page size.
//! @param[out] backingHandle
//!    	Handle to the new memory object (for management)
//! @param[out] frontalHandle
//!    	Handle to the new memory object (for consumers).
HEL_C_LINKAGE HelError helCreateManagedMemory(size_t size, uint32_t flags,
		HelHandle *backingHandle, HelHandle *frontalHandle);

//! Creates memory object that obtains its memory by copy-on-write from another memory object.
//! @param[in] memory
//!    	Handle to the source memory object.
//! @param[in] offset
//!    	Offset in byte relative to @p memory.
//! @param[in] size
//!    	Size of the memory object in bytes.
//!    	Must be aligned to the system's page size.
//! @param[out] handle
//!    	Handle to the new memory object.
HEL_C_LINKAGE HelError helCopyOnWrite(HelHandle memory,
		uintptr_t offset, size_t size, HelHandle *handle);

HEL_C_LINKAGE HelError helAccessPhysical(uintptr_t physical,
		size_t size, HelHandle *handle);

//! Creates a memory object that obtains its memory by delegating to other memory objects.
//! @param[in] numSlots
//! 	Number of slots, i.e., other memory objects that the indirect memory object refers to.
//! @param[out] handle
//!    	Handle to the new memory object.
HEL_C_LINKAGE HelError helCreateIndirectMemory(size_t numSlots, HelHandle *handle);

//! Modifies indirect memory objects.
//!
//! @param[in] indirectHandle
//!    	Handle to the indirect memory object to be modified.
//!    	Must refer to a memory object created by ::helCreateIndirectMemory.
//! @param[in] slotIndex
//!    	Index of the slot to be modified. Must be a non-negative integer smaller than
//!    	@p numSlots (see ::helCreateIndirectMemory).
//! @param[in] memoryHandle
//!    	Handle to the memory object that @p indirectHandle should delegate to.
//! @param[in] offset
//!    	Offset in bytes, relative to @p memoryHandle.
//!    	Must be aligned to the system's page size.
//! @param[in] size
//!    	Size of the indirection in bytes.
//!    	Must be aligned to the system's page size.
HEL_C_LINKAGE HelError helAlterMemoryIndirection(HelHandle indirectHandle, size_t slotIndex,
		HelHandle memoryHandle, uintptr_t offset, size_t size);

HEL_C_LINKAGE HelError helCreateSliceView(HelHandle bundle, uintptr_t offset, size_t size,
		uint32_t flags, HelHandle *handle);

//! Forks memory objects, i.e., copies them using copy-on-write.
//!
//! @param[in] indirectHandle
//!    	Handle to the memory object to be forked.
//!    	Must refer to a memory object created by ::helCopyOnWrite.
//! @param[out] handle
//!    	Handle to the new (i.e., forked) memory object.
HEL_C_LINKAGE HelError helForkMemory(HelHandle handle, HelHandle *forkedHandle);

//! Creates a virtual address space that threads can run in.
//! @param[out] handle
//!     Handle to the new address space.
HEL_C_LINKAGE HelError helCreateSpace(HelHandle *handle);

//! Maps memory objects into an address space.
//! @param[in] memoryHandle
//!     Handle to the memory object.
//! @param[in] spaceHandle
//!     Handle to the address space (see ::helCreateSpace).
//! @param[in] pointer
//!     Pointer to which the memory is mapped.
//!    	Can be specified as @p NULL to let the kernel pick a pointer.
//! @param[in] offset
//!    	Offset in bytes, relative to @p memoryHandle.
//!    	Must be aligned to the system's page size.
//! @param[in] size
//!    	Size of the mappping in bytes.
//!    	Must be aligned to the system's page size.
//! @param[out] actualPointer
//!    	Pointer to which the memory is mapped.
//!     Differs from @p pointer only if @p pointer was specified as @p NULL.
HEL_C_LINKAGE HelError helMapMemory(HelHandle memoryHandle, HelHandle spaceHandle,
		void *pointer, uintptr_t offset, size_t size, uint32_t flags, void **actualPointer);

//! Changes protection attributes of a memory mapping.
//!
//! This is an asynchronous operation.
//! @param[in] spaceHandle
//!     Handle to the address space containing @p pointer.
//! @param[in] pointer
//!     Pointer to the mapping that is modified.
//!    	Must be aligned to the system's page size.
//! @param[in] size
//!    	Size of the mapping that is modified.
//!    	Must be aligned to the system's page size.
HEL_C_LINKAGE HelError helSubmitProtectMemory(HelHandle spaceHandle,
		void *pointer, size_t size, uint32_t flags,
		HelHandle queueHandle, uintptr_t context);

//! Notifies the kernel of dirty pages in a memory mapping.
//!
//! This system call returns after the kernel has scanned all specified pages
//! and determined whether they are dirty
//! or not. It does *not* wait until the pages are clean again.
//!
//! This is an asynchronous operation.
//! @param[in] spaceHandle
//!     Handle to the address space containing @p pointer.
//! @param[in] pointer
//!     Pointer to the mapping that is synchronized.
//!    	Must be aligned to the system's page size.
//! @param[in] size
//!    	Size of the mapping that is synchronized.
//!    	Must be aligned to the system's page size.
HEL_C_LINKAGE HelError helSubmitSynchronizeSpace(HelHandle spaceHandle,
		void *pointer, size_t size,
		HelHandle queueHandle, uintptr_t context);

//! Unmaps memory from an address space.
//!
//! @param[in] spaceHandle
//!     Handle to the address space containing @p pointer.
//! @param[in] pointer
//!     Pointer to the mapping that is unmapped.
//!    	Must be aligned to the system's page size.
//! @param[in] size
//!    	Size of the mapping that is unmapped.
//!    	Must be aligned to the system's page size.
HEL_C_LINKAGE HelError helUnmapMemory(HelHandle spaceHandle, void *pointer, size_t size);

HEL_C_LINKAGE HelError helPointerPhysical(void *pointer, uintptr_t *physical);

//! Load memory (i.e., bytes) from a descriptor.
//!
//! This is an asynchronous operation.
//! @param[in] handle
//!     Handle to the descriptor. This system call supports
//!     address spaces (see ::helCreateAddressSpace)
//!     and virtualized spaces (see ::helCreateVirtualizedSpace).
//! @param[in] address
//!     Address that is accessed, relative to @p handle.
//! @param[in] length
//!     Length of the copied memory region.
HEL_C_LINKAGE HelError helSubmitReadMemory(HelHandle handle, uintptr_t address,
		size_t length, void *buffer,
		HelHandle queue, uintptr_t context);

//! Store memory (i.e., bytes) to a descriptor.
//!
//! This is an asynchronous operation.
//! @param[in] handle
//!     Handle to the descriptor. This system call supports
//!     address spaces (see ::helCreateAddressSpace)
//!     and virtualized spaces (see ::helCreateVirtualizedSpace).
//! @param[in] address
//!     Address that is accessed, relative to @p handle.
//! @param[in] length
//!     Length of the copied memory region.
HEL_C_LINKAGE HelError helSubmitWriteMemory(HelHandle handle, uintptr_t address,
		size_t length, const void *buffer,
		HelHandle queue, uintptr_t context);

HEL_C_LINKAGE HelError helMemoryInfo(HelHandle handle,
		size_t *size);

HEL_C_LINKAGE HelError helSubmitManageMemory(HelHandle handle,
		HelHandle queue, uintptr_t context);

HEL_C_LINKAGE HelError helUpdateMemory(HelHandle handle, int type, uintptr_t offset, size_t length);

HEL_C_LINKAGE HelError helSubmitLockMemoryView(HelHandle handle, uintptr_t offset, size_t size,
		HelHandle queue, uintptr_t context);

//! Notifies the kernel that a certain range of memory should be preloaded.
//!
//! This acts as a hint to the kernel and is meant purely as a performance optimization.
//! The kernel is free to ignore it.
//! @param[in] handle
//!     Handle to the memory object.
//! @param[in] offset
//!     Offset in bytes, relative to @p handle.
//! @param[in] length
//!     Length of the memory range that is preloaded.
HEL_C_LINKAGE HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length);

HEL_C_LINKAGE HelError helCreateVirtualizedSpace(HelHandle *handle);

//! @}
//! @name Thread Management
//! @{

//! Create a new thread.
//! @param[in] universeHandle
//!     Handle to universe of the new thread.
//! @param[in] spaceHandle
//!     Handle to universe of the new thread.
//! @param[in] abi
//!     ABI that the new thread should adhere to.
//! @param[in] ip
//!     Instruction pointer of the new thread.
//! @param[in] sp
//!     Stack pointer of the new thread.
//! @param[out] handle
//!     Handle to the new thread.
HEL_C_LINKAGE HelError helCreateThread(HelHandle universe, HelHandle spaceHandle,
		HelAbi abi, void *ip, void *sp, uint32_t flags, HelHandle *handle);

//! Query run-time statistics of a thread.
//! @param[in] handle
//!     Handle to the thread.
//! @param[out] stats
//!     Statistics related to the thread.
HEL_C_LINKAGE HelError helQueryThreadStats(HelHandle handle, struct HelThreadStats *stats);

//! Set the priority of a thread.
//!
//! Managarm always runs the runnable thread with highest priority.
//! The default priority of a thread is zero.
//! @param[in] handle
//!     Handle to the thread.
//! @param[in] priority
//!     New priority value of the thread.
HEL_C_LINKAGE HelError helSetPriority(HelHandle handle, int priority);

//! Yields the current thread.
HEL_C_LINKAGE HelError helYield();

//! Observe whether a thread changes its state.
//!
//! This is an asynchronous operation.
//! @param[in] handle
//!     Handle to the thread.
//! @param[in] sequence
//!     Previous sequence number.
HEL_C_LINKAGE HelError helSubmitObserve(HelHandle handle, uint64_t sequence,
		HelHandle queue, uintptr_t context);

//! Kill (i.e., terminate) a thread.
//! @param[in] handle
//!     Handle to the thread.
HEL_C_LINKAGE HelError helKillThread(HelHandle handle);

//! Interrupt a thread.
//!
//! This system call temporarily suspends a thread.
//! The thread can later be resumed through the use of ::helResume.
//! @param[in] handle
//!     Handle to the thread.
HEL_C_LINKAGE HelError helInterruptThread(HelHandle handle);

//! Resume a suspended thread.
//!
//! Threads can explicitly be suspended through the use of ::helInterruptThread.
//! They are also suspended on faults and supercalls.
//! @param[in] handle
//!     Handle to the thread.
HEL_C_LINKAGE HelError helResume(HelHandle handle);

//! Load a register image (e.g., from a thread).
//! @param[in] handle
//!     Handle to the thread.
//! @param[in] set
//!     Register set that will be accessed.
//! @param[out] image
//!     Copy of the register image.
HEL_C_LINKAGE HelError helLoadRegisters(HelHandle handle, int set, void *image);

//! Store a register image (e.g., to a thread).
//! @param[in] handle
//!     Handle to the thread.
//! @param[in] set
//!     Register set that will be accessed.
//! @param[in] image
//!     Copy of the register image.
HEL_C_LINKAGE HelError helStoreRegisters(HelHandle handle, int set, const void *image);

HEL_C_LINKAGE HelError helWriteFsBase(void *pointer);

//! Read the system-wide monotone clock.
//!
//! @param[out] counter
//!     Current value of the system-wide clock in nanoseconds since boot.
HEL_C_LINKAGE HelError helGetClock(uint64_t *counter);

//! Wait until time passes.
//!
//! This is an asynchronous operation.
//! @param[in] counter
//!     Deadline (absolute, see ::helGetClock).
//! @param[out] asyncId
//!     ID to identify the asynchronous operation (absolute, see ::helCancelAsync).
HEL_C_LINKAGE HelError helSubmitAwaitClock(uint64_t counter,
		HelHandle queue, uintptr_t context, uint64_t *asyncId);

HEL_C_LINKAGE HelError helCreateVirtualizedCpu(HelHandle handle, HelHandle *out_handle);

HEL_C_LINKAGE HelError helRunVirtualizedCpu(HelHandle handle, struct HelVmexitReason *reason);

HEL_C_LINKAGE HelError helGetRandomBytes(void *buffer, size_t wantedSize, size_t *actualSize);

//! Set a thread's CPU affinity mask.
//! @param[in] handle
//!     Handle to the thread.
//! @param[in] mask
//!     Pointer to a bit mask of CPUs to schedule on.
//! @param[in] size
//!     Size of bit mask.
HEL_C_LINKAGE HelError helSetAffinity(HelHandle thread,
		uint8_t *mask, size_t size);

//! @}
//! @name Message Passing
//! @{

//! Create a stream (which always consists of two lanes).
//! @param[out] lane1
//!     Handle to the first lane of the new stream.
//! @param[out] lane2
//!     Handle to the second lane of the new stream.
HEL_C_LINKAGE HelError helCreateStream(HelHandle *lane1, HelHandle *lane2);

//! Pass messages on a stream.
//! @param[in] handle
//!     Handle to the lane that messages will be passed to.
//! @param[in] actions
//!     Pointer to array of message items.
//! @param[in] count
//!     Number of elements in @p actions.
HEL_C_LINKAGE HelError helSubmitAsync(HelHandle handle, const struct HelAction *actions,
		size_t count, HelHandle queue, uintptr_t context, uint32_t flags);

HEL_C_LINKAGE HelError helShutdownLane(HelHandle handle);

//! @}
//! @name Inter-Thread Synchronization
//! @{

//! Waits on a futex.
//! @param[in] pointer
//!     Pointer that identifies the futex.
//! @param[in] expected
//!     Expected value of the futex. This function does nothing when the
//!     futex pointed to by @pointer matches this value.
//! @param[in] deadline
//!     Timeout (in absolute monotone time, see ::helGetClock).
HEL_C_LINKAGE HelError helFutexWait(int *pointer, int expected, int64_t deadline);

//! Wakes up all waiters of a futex.
//! @param[in] pointer
//!     Pointer that identifies the futex.
HEL_C_LINKAGE HelError helFutexWake(int *pointer);

//! @}
//! @name Event Handling
//! @{

//! Create an event that fires at most once.
//! @param[out] handle
//!     Handle to the new event.
HEL_C_LINKAGE HelError helCreateOneshotEvent(HelHandle *handle);

//! Create an event consisting of multiple bits that can fire independently.
//! @param[out] handle
//!     Handle to the new event.
HEL_C_LINKAGE HelError helCreateBitsetEvent(HelHandle *handle);

//! Raise an event.
//! @param[in] handle
//!     Handle to the event that will be raised.
HEL_C_LINKAGE HelError helRaiseEvent(HelHandle handle);

HEL_C_LINKAGE HelError helAccessIrq(int number, HelHandle *handle);

HEL_C_LINKAGE HelError helAcknowledgeIrq(HelHandle handle, uint32_t flags, uint64_t sequence);

//! Wait for an event.
//!
//! This is an asynchronous operation.
//! @param[in] handle
//!     Handle to the event that will be awaited.
//! @param[in] sequence
//!     Previous sequence number.
HEL_C_LINKAGE HelError helSubmitAwaitEvent(HelHandle handle, uint64_t sequence,
		HelHandle queue, uintptr_t context);

HEL_C_LINKAGE HelError helAutomateIrq(HelHandle handle, uint32_t flags, HelHandle kernlet);

//! @}
//! @name Input/Output
//! @{

HEL_C_LINKAGE HelError helAccessIo(uintptr_t *port_array, size_t num_ports,
		HelHandle *handle);

//! Enable userspace access to hardware I/O resources.
//! @param[in] handle
//!     Handle to the hardware I/O resource.
HEL_C_LINKAGE HelError helEnableIo(HelHandle handle);

HEL_C_LINKAGE HelError helEnableFullIo();

//! @}
//! @name Kernlet Management
//! @{

//! Bind parameters to a kernlet.
//! @param[in] handle
//!     Handle to the unbound kernlet.
//! @param[in] data
//!     Pointer to an array of binding parameters.
//! @param[in] numData
//!     Number of binding parameters in @p data.
//! @param[out] boundHandle
//!     Handle to the bound kernlet.
HEL_C_LINKAGE HelError helBindKernlet(HelHandle handle,
		const union HelKernletData *data, size_t numData, HelHandle *boundHandle);

//! @}

extern inline __attribute__ (( always_inline )) const char *_helErrorString(HelError code) {
	switch(code) {
	case kHelErrNone:
		return "Success";
	case kHelErrIllegalSyscall:
		return "Illegal syscall";
	case kHelErrIllegalArgs:
		return "Illegal arguments";
	case kHelErrIllegalState:
		return "Illegal state";
	case kHelErrUnsupportedOperation:
		return "Unsupported operation";
	case kHelErrNoDescriptor:
		return "No such descriptor";
	case kHelErrBadDescriptor:
		return "Illegal descriptor for this operation";
	case kHelErrThreadTerminated:
		return "Thread terminated already";
	case kHelErrLaneShutdown:
		return "Lane shutdown";
	case kHelErrEndOfLane:
		return "End of lane";
	case kHelErrBufferTooSmall:
		return "Buffer too small";
	case kHelErrQueueTooSmall:
		return "Buffer too small";
	case kHelErrFault:
		return "Segfault";
	case kHelErrNoHardwareSupport:
		return "Missing hardware support for this feature";
	case kHelErrNoMemory:
		return "Out of memory";
	case kHelErrTransmissionMismatch:
		return "Transmission mismatch";
	case kHelErrCancelled:
		return "Cancelled";
	case kHelErrOutOfBounds:
		return "Cancelled";
	default:
		return 0;
	}
}

extern inline __attribute__ (( always_inline )) void _helCheckFailed(HelError err_code,
		const char *string, int fatal) {
	helLog(string, strlen(string));

	const char *err_string = _helErrorString(err_code);
	if(err_string == 0)
		err_string = "(Unexpected error code)";
	helLog(err_string, strlen(err_string));
	helLog("\n", 1);

	if(fatal)
		helPanic(0, 0);
}

#define HEL_STRINGIFY_AUX(x) #x
#define HEL_STRINGIFY(x) HEL_STRINGIFY_AUX(x)

#define HEL_CHECK(expr) do { HelError __error = expr; if(__error != kHelErrNone) \
		_helCheckFailed(__error, "HEL_CHECK failed: " #expr "\n" \
		"    In file " __FILE__ " on line " HEL_STRINGIFY(__LINE__) "\n", 1); } while(0)
#define HEL_SOFT_CHECK(expr) do { HelError __error = expr; if(__error != kHelErrNone) \
		_helCheckFailed(__error, "HEL_SOFT_CHECK failed: " #expr "\n" \
		"    In file " __FILE__ " on line " HEL_STRINGIFY(__LINE__) "\n", 0); } while(0)

#endif // HEL_H

