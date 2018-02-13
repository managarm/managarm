
//! @file hel.h

#ifndef HEL_H
#define HEL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
#define HEL_C_LINKAGE extern "C"
#else
#define HEL_C_LINKAGE
#endif

enum {
	// largest system call number plus 1
	kHelNumCalls = 83,

	kHelCallLog = 1,
	kHelCallPanic = 10,

	kHelCallCreateUniverse = 62,
	kHelCallTransferDescriptor = 66,
	kHelCallDescriptorInfo = 32,
	kHelCallCloseDescriptor = 20,

	kHelCallAllocateMemory = 2,
	kHelCallCreateManagedMemory = 64,
	kHelCallAccessPhysical = 30,
	kHelCallCreateSpace = 27,
	kHelCallForkSpace = 33,
	kHelCallMapMemory = 44,
	kHelCallUnmapMemory = 36,
	kHelCallPointerPhysical = 43,
	kHelCallLoadForeign = 77,
	kHelCallStoreForeign = 78,
	kHelCallMemoryInfo = 26,
	kHelCallSubmitManageMemory = 46,
	kHelCallCompleteLoad = 47,
	kHelCallSubmitLockMemory = 48,
	kHelCallLoadahead = 49,
	
	kHelCallCreateThread = 67,
	kHelCallYield = 34,
	kHelCallSubmitObserve = 74,
	kHelCallResume = 61,
	kHelCallLoadRegisters = 75,
	kHelCallStoreRegisters = 76,
	kHelCallWriteFsBase = 41,
	kHelCallGetClock = 42,
	kHelCallSubmitAwaitClock = 80,
	
	kHelCallCreateStream = 68,
	kHelCallSubmitAsync = 79,

	kHelCallFutexWait = 70,
	kHelCallFutexWake = 71,
	
	kHelCallAccessIrq = 14,
	kHelCallAcknowledgeIrq = 81,
	kHelCallSubmitAwaitEvent = 82,

	kHelCallAccessIo = 11,
	kHelCallEnableIo = 12,
	kHelCallEnableFullIo = 35,
	
	kHelCallSuper = 0x80000000
};

enum {
	kHelErrNone = 0,
	kHelErrIllegalSyscall = 5,
	kHelErrIllegalArgs = 7,
	kHelErrNoDescriptor = 4,
	kHelErrBadDescriptor = 2,
	kHelErrClosedLocally = 8,
	kHelErrClosedRemotely = 9,
	kHelErrBufferTooSmall = 1,
	kHelErrFault = 10,
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
	kHelActionSendFromBuffer = 1,
	kHelActionRecvInline = 7,
	kHelActionRecvToBuffer = 3,
	kHelActionPushDescriptor = 2,
	kHelActionPullDescriptor = 4
};

enum {
	kHelItemChain = 1,
	kHelItemAncillary = 2
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

enum HelMapFlags {
	kHelMapProtRead = 256,
	kHelMapProtWrite = 512,
	kHelMapProtExecute = 1024,
	kHelMapDropAtFork = 32,
	kHelMapShareAtFork = 8,
	kHelMapCopyOnWriteAtFork = 64,
	kHelMapDontRequireBacking = 128
};

enum HelThreadFlags {
	kHelThreadExclusive = 2,
	kHelThreadTrapsAreFatal = 8,
	kHelThreadStopped = 9
};

enum HelObservation {
	kHelObserveNull = 0,
	kHelObserveStop = 4,
	kHelObservePanic = 3,
	kHelObserveBreakpoint = 1,
	kHelObserveGeneralFault = 5,
	kHelObservePageFault = 2,
	kHelObserveSuperCall = 0x80000000
};

enum HelRegisterSets {
	kHelRegsProgram = 1,
	kHelRegsGeneral = 2,
	kHelRegsThread = 3,
	kHelRegsDebug = 4
};

enum HelMessageFlags {
	kHelRequest = 1,
	kHelResponse = 2
};

//! Flag for kernelState; signals that there are waiters.
static const unsigned int kHelQueueWaiters = (unsigned int)1 << 31;

//! Flag for kernelState; signals that the kernel needs a next HelQueue.
static const unsigned int kHelQueueWantNext = (unsigned int)1 << 30;

//! Bit mask for kernelState; the kernel enqueue pointer.
static const unsigned int kHelQueueTail = ((unsigned int)1 << 30) - 1;

//! Flag for userState; signals that there is a next HelQueue.
static const unsigned int kHelQueueHasNext = (unsigned int)1 << 31;

//! In-memory kernel-user queue.
//! Each element in the queue is prefixed by a HelElement struct.
struct HelQueue {
	//! Maximum size of a single element in bytes.
	//! Constant. Does not include the per-element HelElement header.
	unsigned int elementLimit;

	//! Size of the whole queue in bytes.
	//! Constant. Does not include the HelQueue header.
	unsigned int queueLength;

	//! The Futex user space waits on.
	//! Must be accessed atomically.
	unsigned int kernelState;

	//! The Futex the kernel waits on.
	//! Must be accessed atomically.
	unsigned int userState;

	//! Pointer to the next queue.
	//! Supplied by user space.
	//! The kernel requests a next queue by setting the kHelQueueWantNext bit.
	//! Must be accessed atomically.
	struct HelQueue *nextQueue;

	char queueBuffer[];
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

struct HelManageResult {
	HelError error;
	int reserved;
	uintptr_t offset;
	size_t length;
};

struct HelObserveResult {
	HelError error;
	unsigned int observation;
	uint64_t code;
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
	int reserved;
	uint64_t sequence;
};

enum HelIrqFlags {
	kHelIrqExclusive = 1,
	kHelIrqManualAcknowledge = 2
};

HEL_C_LINKAGE HelError helLog(const char *string, size_t length);
HEL_C_LINKAGE void helPanic(const char *string, size_t length)
		__attribute__ (( noreturn ));

HEL_C_LINKAGE HelError helCreateUniverse(HelHandle *handle);
HEL_C_LINKAGE HelError helTransferDescriptor(HelHandle handle, HelHandle universe_handle,
		HelHandle *out_handle);
HEL_C_LINKAGE HelError helDescriptorInfo(HelHandle handle, struct HelDescriptorInfo *info);
HEL_C_LINKAGE HelError helCloseDescriptor(HelHandle handle);

HEL_C_LINKAGE HelError helAllocateMemory(size_t size, uint32_t flags, HelHandle *handle);
HEL_C_LINKAGE HelError helCreateManagedMemory(size_t size, uint32_t flags,
		HelHandle *backing_handle, HelHandle *frontal_handle);
HEL_C_LINKAGE HelError helAccessPhysical(uintptr_t physical,
		size_t size, HelHandle *handle);
HEL_C_LINKAGE HelError helCreateSpace(HelHandle *handle);
HEL_C_LINKAGE HelError helForkSpace(HelHandle handle, HelHandle *forked);
HEL_C_LINKAGE HelError helMapMemory(HelHandle handle, HelHandle space,
		void *pointer, uintptr_t offset, size_t size, uint32_t flags, void **actual_pointer);
HEL_C_LINKAGE HelError helUnmapMemory(HelHandle space, void *pointer, size_t size);
HEL_C_LINKAGE HelError helPointerPhysical(void *pointer, uintptr_t *physical);
HEL_C_LINKAGE HelError helLoadForeign(HelHandle handle, uintptr_t address,
		size_t length, void *buffer);
HEL_C_LINKAGE HelError helStoreForeign(HelHandle handle, uintptr_t address,
		size_t length, const void *buffer);
HEL_C_LINKAGE HelError helMemoryInfo(HelHandle handle,
		size_t *size);
HEL_C_LINKAGE HelError helSubmitManageMemory(HelHandle handle,
		struct HelQueue *queue, uintptr_t context);
HEL_C_LINKAGE HelError helCompleteLoad(HelHandle handle, uintptr_t offset, size_t length);
HEL_C_LINKAGE HelError helSubmitLockMemory(HelHandle handle, uintptr_t offset, size_t size,
		struct HelQueue *queue, uintptr_t context);
HEL_C_LINKAGE HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length);

HEL_C_LINKAGE HelError helCreateThread(HelHandle universe, HelHandle address_space,
		HelAbi abi, void *ip, void *sp, uint32_t flags, HelHandle *handle);
HEL_C_LINKAGE HelError helYield();
HEL_C_LINKAGE HelError helSubmitObserve(HelHandle handle,
		struct HelQueue *queue, uintptr_t context);
HEL_C_LINKAGE HelError helResume(HelHandle handle);
HEL_C_LINKAGE HelError helLoadRegisters(HelHandle handle, int set, void *image); 
HEL_C_LINKAGE HelError helStoreRegisters(HelHandle handle, int set, const void *image);
HEL_C_LINKAGE HelError helWriteFsBase(void *pointer);
HEL_C_LINKAGE HelError helGetClock(uint64_t *counter);
HEL_C_LINKAGE HelError helSubmitAwaitClock(uint64_t counter,
		struct HelQueue *queue, uintptr_t context);

HEL_C_LINKAGE HelError helCreateStream(HelHandle *lane1, HelHandle *lane2);
HEL_C_LINKAGE HelError helSubmitAsync(HelHandle handle, const HelAction *actions,
		size_t count, struct HelQueue *queue, uintptr_t context, uint32_t flags);

HEL_C_LINKAGE HelError helFutexWait(int *pointer, int expected);
HEL_C_LINKAGE HelError helFutexWake(int *pointer);

HEL_C_LINKAGE HelError helAccessIrq(int number, HelHandle *handle);
HEL_C_LINKAGE HelError helAcknowledgeIrq(HelHandle handle, uint32_t flags, uint64_t sequence);
HEL_C_LINKAGE HelError helSubmitAwaitEvent(HelHandle handle, uint64_t sequence,
		struct HelQueue *queue, uintptr_t context);

HEL_C_LINKAGE HelError helAccessIo(uintptr_t *port_array, size_t num_ports,
		HelHandle *handle);
HEL_C_LINKAGE HelError helEnableIo(HelHandle handle);
HEL_C_LINKAGE HelError helEnableFullIo();

extern inline __attribute__ (( always_inline )) const char *_helErrorString(HelError code) {
	switch(code) {
	case kHelErrNone:
		return "Success";
	case kHelErrIllegalSyscall:
		return "Illegal syscall";
	case kHelErrIllegalArgs:
		return "Illegal arguments";
	case kHelErrNoDescriptor:
		return "No such descriptor";
	case kHelErrBadDescriptor:
		return "Illegal descriptor for this operation";
	case kHelErrClosedLocally:
		return "Resource closed locally";
	case kHelErrClosedRemotely:
		return "Resource closed remotely";
	case kHelErrBufferTooSmall:
		return "Buffer too small";
	case kHelErrFault:
		return "Segfault";
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

