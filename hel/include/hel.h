
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
	kHelNumCalls = 100,

	kHelCallLog = 1,
	kHelCallPanic = 10,

	kHelCallCreateUniverse = 62,
	kHelCallTransferDescriptor = 66,
	kHelCallDescriptorInfo = 32,
	kHelCallGetCredentials = 84,
	kHelCallCloseDescriptor = 20,

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
	kHelCallForkSpace = 33,
	kHelCallMapMemory = 44,
	kHelCallSubmitProtectMemory = 99,
	kHelCallUnmapMemory = 36,
	kHelCallPointerPhysical = 43,
	kHelCallLoadForeign = 77,
	kHelCallStoreForeign = 78,
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

	kHelCallCreateStream = 68,
	kHelCallSubmitAsync = 79,
	kHelCallShutdownLane = 91,

	kHelCallFutexWait = 70,
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
	kHelErrClosedLocally = 8, // Deprecated name.
	kHelErrEndOfLane = 9,
	kHelErrClosedRemotely = 9, // Deprecated name.
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

	HelX86SegmentRegister cs, ds, es, fs, gs, ss;
	HelX86SegmentRegister tr, ldt;
	HelX86DescriptorTable gdt, idt;

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
	// Basic mapping modes. One of these flags needs to be set.
	kHelMapShareAtFork = 8,
	kHelMapCopyOnWrite = 16,

	// Additional flags that may be set.
	kHelMapProtRead = 256,
	kHelMapProtWrite = 512,
	kHelMapProtExecute = 1024,
	kHelMapDropAtFork = 32,
	kHelMapCopyOnWriteAtFork = 64,
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
	
	kHelRegIp = 0,
	kHelRegSp = 1
};

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
  khelVmexitError = -1,
};

struct HelVmexitReason {
	uint32_t exitReason;
};

HEL_C_LINKAGE HelError helLog(const char *string, size_t length);
HEL_C_LINKAGE void helPanic(const char *string, size_t length)
		__attribute__ (( noreturn ));

HEL_C_LINKAGE HelError helCreateUniverse(HelHandle *handle);
HEL_C_LINKAGE HelError helTransferDescriptor(HelHandle handle, HelHandle universe_handle,
		HelHandle *out_handle);
HEL_C_LINKAGE HelError helDescriptorInfo(HelHandle handle, struct HelDescriptorInfo *info);
HEL_C_LINKAGE HelError helGetCredentials(HelHandle handle, uint32_t flags,
		char *credentials);
HEL_C_LINKAGE HelError helCloseDescriptor(HelHandle handle);

//! size_shift:    Size of the indexQueue array.
//! element_limit: Maximum size of a single element in bytes.
//!                Does not include the per-element HelElement header.
HEL_C_LINKAGE HelError helCreateQueue(HelQueue *head, uint32_t flags,
		unsigned int size_shift, size_t element_limit, HelHandle *handle);
HEL_C_LINKAGE HelError helSetupChunk(HelHandle queue, int index, HelChunk *chunk, uint32_t flags);
HEL_C_LINKAGE HelError helCancelAsync(HelHandle queue, uint64_t async_id);

HEL_C_LINKAGE HelError helAllocateMemory(size_t size, uint32_t flags,
		struct HelAllocRestrictions *restrictions, HelHandle *handle);
HEL_C_LINKAGE HelError helResizeMemory(HelHandle handle, size_t new_size);
HEL_C_LINKAGE HelError helCreateManagedMemory(size_t size, uint32_t flags,
		HelHandle *backing_handle, HelHandle *frontal_handle);
HEL_C_LINKAGE HelError helCopyOnWrite(HelHandle memory,
		uintptr_t offset, size_t size, HelHandle *handle);
HEL_C_LINKAGE HelError helAccessPhysical(uintptr_t physical,
		size_t size, HelHandle *handle);
HEL_C_LINKAGE HelError helCreateIndirectMemory(size_t numSlots, HelHandle *handle);
HEL_C_LINKAGE HelError helAlterMemoryIndirection(HelHandle indirectHandle, size_t slotIndex,
		HelHandle memoryHandle, uintptr_t offset, size_t size);
HEL_C_LINKAGE HelError helCreateSliceView(HelHandle bundle, uintptr_t offset, size_t size,
		uint32_t flags, HelHandle *handle);
HEL_C_LINKAGE HelError helForkMemory(HelHandle handle, HelHandle *forked);
HEL_C_LINKAGE HelError helCreateSpace(HelHandle *handle);
HEL_C_LINKAGE HelError helForkSpace(HelHandle handle, HelHandle *forked);
HEL_C_LINKAGE HelError helMapMemory(HelHandle handle, HelHandle space,
		void *pointer, uintptr_t offset, size_t size, uint32_t flags, void **actual_pointer);
HEL_C_LINKAGE HelError helSubmitProtectMemory(HelHandle space,
		void *pointer, size_t size, uint32_t flags,
		HelHandle queue, uintptr_t context);
HEL_C_LINKAGE HelError helUnmapMemory(HelHandle space, void *pointer, size_t size);
HEL_C_LINKAGE HelError helPointerPhysical(void *pointer, uintptr_t *physical);
HEL_C_LINKAGE HelError helLoadForeign(HelHandle handle, uintptr_t address,
		size_t length, void *buffer);
HEL_C_LINKAGE HelError helStoreForeign(HelHandle handle, uintptr_t address,
		size_t length, const void *buffer);
HEL_C_LINKAGE HelError helMemoryInfo(HelHandle handle,
		size_t *size);
HEL_C_LINKAGE HelError helSubmitManageMemory(HelHandle handle,
		HelHandle queue, uintptr_t context);
HEL_C_LINKAGE HelError helUpdateMemory(HelHandle handle, int type, uintptr_t offset, size_t length);
HEL_C_LINKAGE HelError helSubmitLockMemoryView(HelHandle handle, uintptr_t offset, size_t size,
		HelHandle queue, uintptr_t context);
HEL_C_LINKAGE HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length);
HEL_C_LINKAGE HelError helCreateVirtualizedSpace(HelHandle *handle);

HEL_C_LINKAGE HelError helCreateThread(HelHandle universe, HelHandle address_space,
		HelAbi abi, void *ip, void *sp, uint32_t flags, HelHandle *handle);
HEL_C_LINKAGE HelError helQueryThreadStats(HelHandle handle, HelThreadStats *stats);
HEL_C_LINKAGE HelError helSetPriority(HelHandle handle, int priority);
HEL_C_LINKAGE HelError helYield();
HEL_C_LINKAGE HelError helSubmitObserve(HelHandle handle, uint64_t in_seq,
		HelHandle queue, uintptr_t context);
HEL_C_LINKAGE HelError helKillThread(HelHandle handle);
HEL_C_LINKAGE HelError helInterruptThread(HelHandle handle);
HEL_C_LINKAGE HelError helResume(HelHandle handle);
HEL_C_LINKAGE HelError helLoadRegisters(HelHandle handle, int set, void *image);
HEL_C_LINKAGE HelError helStoreRegisters(HelHandle handle, int set, const void *image);
HEL_C_LINKAGE HelError helWriteFsBase(void *pointer);
HEL_C_LINKAGE HelError helGetClock(uint64_t *counter);
HEL_C_LINKAGE HelError helSubmitAwaitClock(uint64_t counter,
		HelHandle queue, uintptr_t context, uint64_t *async_id);
HEL_C_LINKAGE HelError helCreateVirtualizedCpu(HelHandle handle, HelHandle *out_handle);
HEL_C_LINKAGE HelError helRunVirtualizedCpu(HelHandle handle, HelVmexitReason *reason);

HEL_C_LINKAGE HelError helCreateStream(HelHandle *lane1, HelHandle *lane2);
HEL_C_LINKAGE HelError helSubmitAsync(HelHandle handle, const HelAction *actions,
		size_t count, HelHandle queue, uintptr_t context, uint32_t flags);
HEL_C_LINKAGE HelError helShutdownLane(HelHandle handle);

HEL_C_LINKAGE HelError helFutexWait(int *pointer, int expected);
HEL_C_LINKAGE HelError helFutexWake(int *pointer);

HEL_C_LINKAGE HelError helCreateOneshotEvent(HelHandle *handle);
HEL_C_LINKAGE HelError helCreateBitsetEvent(HelHandle *handle);
HEL_C_LINKAGE HelError helRaiseEvent(HelHandle handle);
HEL_C_LINKAGE HelError helAccessIrq(int number, HelHandle *handle);
HEL_C_LINKAGE HelError helAcknowledgeIrq(HelHandle handle, uint32_t flags, uint64_t sequence);
HEL_C_LINKAGE HelError helSubmitAwaitEvent(HelHandle handle, uint64_t sequence,
		HelHandle queue, uintptr_t context);
HEL_C_LINKAGE HelError helAutomateIrq(HelHandle handle, uint32_t flags, HelHandle kernlet);

HEL_C_LINKAGE HelError helAccessIo(uintptr_t *port_array, size_t num_ports,
		HelHandle *handle);
HEL_C_LINKAGE HelError helEnableIo(HelHandle handle);
HEL_C_LINKAGE HelError helEnableFullIo();

HEL_C_LINKAGE HelError helBindKernlet(HelHandle handle,
		const HelKernletData *data, size_t num_data, HelHandle *bound_handle);

extern inline __attribute__ (( always_inline )) const char *_helErrorString(HelError code) {
	switch(code) {
	case kHelErrNone:
		return "Success";
	case kHelErrIllegalSyscall:
		return "Illegal syscall";
	case kHelErrIllegalArgs:
		return "Illegal arguments";
	case kHelErrUnsupportedOperation:
		return "Unsupported operation";
	case kHelErrNoDescriptor:
		return "No such descriptor";
	case kHelErrBadDescriptor:
		return "Illegal descriptor for this operation";
	case kHelErrLaneShutdown:
		return "Lane shutdown";
	case kHelErrEndOfLane:
		return "End of lane";
	case kHelErrBufferTooSmall:
		return "Buffer too small";
	case kHelErrFault:
		return "Segfault";
	case kHelErrNoHardwareSupport:
		return "Missing hardware support for this feature";
	case kHelErrNoMemory:
		return "Out of memory";
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

