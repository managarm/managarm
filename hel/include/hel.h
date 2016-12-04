
#ifndef HEL_H
#define HEL_H

#ifdef __cplusplus
#define HEL_C_LINKAGE extern "C"
#else
#define HEL_C_LINKAGE
#endif

enum {
	// largest system call number plus 1
	kHelNumCalls = 74,

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
	kHelCallMemoryInfo = 26,
	kHelCallSubmitProcessLoad = 46,
	kHelCallCompleteLoad = 47,
	kHelCallSubmitLockMemory = 48,
	kHelCallLoadahead = 49,
	
	kHelCallCreateThread = 67,
	kHelCallYield = 34,
	kHelCallSubmitObserve = 60,
	kHelCallResume = 61,
	kHelCallExitThisThread = 5,
	kHelCallWriteFsBase = 41,
	kHelCallGetClock = 42,

	kHelCallCreateEventHub = 13,
	kHelCallWaitForEvents = 45,
	kHelCallWaitForCertainEvent = 65,
	
	kHelCallCreateStream = 68,
	kHelCallSubmitAsync = 72,

	kHelCallFutexWait = 70,
	kHelCallFutexWake = 71,
	
	kHelCallAccessIrq = 14,
	kHelCallSetupIrq = 51,
	kHelCallAcknowledgeIrq = 50,
	kHelCallSubmitWaitForIrq = 73,
	kHelCallSubscribeIrq = 52,

	kHelCallAccessIo = 11,
	kHelCallEnableIo = 12,
	kHelCallEnableFullIo = 35,
	
	kHelCallControlKernel = 31
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
};

typedef int HelError;
typedef int HelAbi;
typedef int64_t HelHandle;
typedef int64_t HelNanotime;

enum {
	kHelNullHandle = 0,
	kHelThisUniverse = -1
};

enum {
	kHelAnyRequest = -1,
	kHelAnySequence = -1,
	kHelNoFunction = 0,
	kHelNoObject = 0,
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
	kHelEventLoadMemory = 7,
	kHelEventLockMemory = 8,
	kHelEventOffer = 13,
	kHelEventAccept = 14,
	kHelEventObserve = 12,
	kHelEventSendString = 11,
	kHelEventSendDescriptor = 10,
	kHelEventRecvString = 1,
	kHelEventRecvStringToQueue = 9,
	kHelEventRecvDescriptor = 5,
	kHelEventIrq = 4
};

enum {
	kHelItemChain = 1,
	kHelItemAncillary = 2
};

struct HelAction {
	int type;
	uintptr_t context;
	uint32_t flags;
	// TODO: the following fields could be put into unions
	void *buffer;
	size_t length;
	HelHandle handle;
};

struct HelEvent {
	int type;
	HelError error;

	int64_t msgRequest;
	int64_t msgSequence;
	size_t length;
	HelHandle handle;

	// used by kHelEventLoadMemory
	size_t offset;

	int64_t asyncId;
	uintptr_t submitFunction;
	uintptr_t submitObject;
};

enum {
	kHelDescMemory = 1,
	kHelDescAddressSpace = 2,
	kHelDescThread = 3,
	kHelDescEventHub = 4,
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
	kHelMapReadOnly = 1,
	kHelMapReadWrite = 2,
	kHelMapReadExecute = 4,
	kHelMapDropAtFork = 32,
	kHelMapShareAtFork = 8,
	kHelMapCopyOnWriteAtFork = 64,
	kHelMapDontRequireBacking = 128
};

enum HelThreadFlags {
	kHelThreadExclusive = 2,
	kHelThreadTrapsAreFatal = 8
};

enum HelMessageFlags {
	kHelRequest = 1,
	kHelResponse = 2
};

struct HelRingBuffer {
	uint32_t refCount;

	char data[];
};

//! Flag for kernelState; signals that there are waiters.
static const unsigned int kHelQueueWaiters = (unsigned int)1 << 31;

//! Flag for kernelState; signals that the kernel needs a next HelQueue.
static const unsigned int kHelQueueWantNext = (unsigned int)1 << 30;

//! Bit mask for kernelState; the kernel enqueue pointer.
static const unsigned int kHelQueueTail = ((unsigned int)1 << 30) - 1;

//! Flag for userState; signals that there is a next HelQueue.
static const unsigned int kHelQueueHasNext = (unsigned int)1 << 31;

struct HelQueue {
	//! Maximum size of a single element in bytes.
	//! Constant.
	unsigned int elementLimit;

	//! Size of the whole queue (without header) in bytes.
	//! Constant.
	unsigned int queueLength;

	//! This is the Futex user space waits on.
	//! Must be accessed atomically.
	unsigned int kernelState;

	//! This is the Futex the kernel waits on.
	//! Must be accessed atomically.
	unsigned int userState;

	//! Pointer to the next queue.
	//! Supplied by user space.
	//! The kernel requests a next queue by setting the kHelQueueWantNext bit.
	//! Must be accessed atomically.
	struct HelQueue *nextQueue;

	char queueBuffer[];
};

struct HelElement {
	unsigned int length;
	unsigned int reserved;
	void *context;
};

struct HelSimpleResult {
	HelError error;
	int reserved;
};

struct HelInlineResult {
	HelError error;
	int reserved;
	size_t length;
	char data[];
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
HEL_C_LINKAGE HelError helMemoryInfo(HelHandle handle,
		size_t *size);
HEL_C_LINKAGE HelError helSubmitProcessLoad(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id);
HEL_C_LINKAGE HelError helCompleteLoad(HelHandle handle, uintptr_t offset, size_t length);
HEL_C_LINKAGE HelError helSubmitLockMemory(HelHandle handle, HelHandle hub_handle,
		uintptr_t offset, size_t size,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id);
HEL_C_LINKAGE HelError helLoadahead(HelHandle handle, uintptr_t offset, size_t length);

HEL_C_LINKAGE HelError helCreateThread(HelHandle universe, HelHandle address_space,
		HelAbi abi, void *ip, void *sp, uint32_t flags, HelHandle *handle);
HEL_C_LINKAGE HelError helYield();
HEL_C_LINKAGE HelError helSubmitObserve(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id);
HEL_C_LINKAGE HelError helResume(HelHandle handle);
HEL_C_LINKAGE HelError helExitThisThread();
HEL_C_LINKAGE HelError helWriteFsBase(void *pointer);
HEL_C_LINKAGE HelError helGetClock(uint64_t *counter);

HEL_C_LINKAGE HelError helCreateEventHub(HelHandle *handle);
HEL_C_LINKAGE HelError helWaitForEvents(HelHandle handle,
		struct HelEvent *list, size_t max_items,
		HelNanotime max_time, size_t *num_items);
HEL_C_LINKAGE HelError helWaitForCertainEvent(HelHandle handle,
		int64_t async_id, struct HelEvent *event, HelNanotime max_time);

HEL_C_LINKAGE HelError helCreateStream(HelHandle *lane1, HelHandle *lane2);
HEL_C_LINKAGE HelError helSubmitAsync(HelHandle handle, const HelAction *actions,
		size_t count, struct HelQueue *queue, uint32_t flags);

HEL_C_LINKAGE HelError helFutexWait(int *pointer, int expected);
HEL_C_LINKAGE HelError helFutexWake(int *pointer);

HEL_C_LINKAGE HelError helAccessIrq(int number, HelHandle *handle);
HEL_C_LINKAGE HelError helSetupIrq(HelHandle handle, uint32_t flags);
HEL_C_LINKAGE HelError helAcknowledgeIrq(HelHandle handle);
HEL_C_LINKAGE HelError helSubmitWaitForIrq(HelHandle handle,
		struct HelQueue *queue, uintptr_t context);
HEL_C_LINKAGE HelError helSubscribeIrq(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id);

HEL_C_LINKAGE HelError helAccessIo(uintptr_t *port_array, size_t num_ports,
		HelHandle *handle);
HEL_C_LINKAGE HelError helEnableIo(HelHandle handle);
HEL_C_LINKAGE HelError helEnableFullIo();

HEL_C_LINKAGE HelError helControlKernel(int subsystem, int interface,
		const void *input, void *output);

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

