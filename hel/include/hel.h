
#ifdef __cplusplus
#define HEL_C_LINKAGE extern "C"
#else
#define HEL_C_LINKAGE
#endif

enum {
	kHelCallLog = 1,
	kHelCallPanic = 10,

	kHelCallCloseDescriptor = 20,

	kHelCallAllocateMemory = 2,
	kHelCallAccessPhysical = 30,
	kHelCallCreateSpace = 27,
	kHelCallMapMemory = 6,
	kHelCallMemoryInfo = 26,
	
	kHelCallCreateThread = 3,
	kHelCallExitThisThread = 5,
	
	kHelCallCreateEventHub = 13,
	kHelCallWaitForEvents = 16,

	kHelCallCreateBiDirectionPipe = 4,
	kHelCallSendString = 8,
	kHelCallSendDescriptor = 28,
	kHelCallSubmitRecvString = 9,
	kHelCallSubmitRecvDescriptor = 29,
	
	kHelCallCreateServer = 17,
	kHelCallSubmitAccept = 18,
	kHelCallSubmitConnect = 19,

	kHelCallCreateRd = 21,
	kHelCallRdMount = 25,
	kHelCallRdPublish = 22,
	kHelCallRdUnlink = 24,
	kHelCallRdOpen = 23,

	kHelCallAccessIrq = 14,
	kHelCallSubmitWaitForIrq = 15,

	kHelCallAccessIo = 11,
	kHelCallEnableIo = 12
};

enum {
	kHelErrNone = 0,
	kHelErrBufferTooSmall = 1
};

typedef int HelError;
typedef uint64_t HelHandle;
typedef int64_t HelNanotime;

enum {
	kHelNullHandle = 0,
	kHelAnyRequest = -1,
	kHelAnySequence = -1,
	kHelNoFunction = 0,
	kHelNoObject = 0,
	kHelWaitInfinite = -1
};

struct HelThreadState {
	uint64_t rax, rbx, rcx, rdx;
	uint64_t rsi, rdi, rbp;
	uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
	uint64_t rsp, rip, rflags;
};

enum {
	kHelEventRecvString = 1,
	kHelEventRecvDescriptor = 5,
	kHelEventAccept = 2,
	kHelEventConnect = 3,
	kHelEventIrq = 4
};

struct HelEvent {
	int type;
	HelError error;

	int64_t msgRequest;
	int64_t msgSequence;
	size_t length;
	HelHandle handle;

	int64_t asyncId;
	uintptr_t submitFunction;
	uintptr_t submitObject;
};

enum HelMapFlags {
	kHelMapReadOnly = 1,
	kHelMapReadWrite = 2,
	kHelMapReadExecute = 4
};

HEL_C_LINKAGE HelError helLog(const char *string, size_t length);
HEL_C_LINKAGE void helPanic(const char *string, size_t length)
		__attribute__ (( noreturn ));

HEL_C_LINKAGE HelError helCloseDescriptor(HelHandle handle);

HEL_C_LINKAGE HelError helAllocateMemory(size_t size, HelHandle *handle);
HEL_C_LINKAGE HelError helAccessPhysical(uintptr_t physical,
		size_t size, HelHandle *handle);
HEL_C_LINKAGE HelError helCreateSpace(HelHandle *handle);
HEL_C_LINKAGE HelError helMapMemory(HelHandle handle, HelHandle space,
		void *pointer, size_t size, uint32_t flags, void **actual_pointer);
HEL_C_LINKAGE HelError helMemoryInfo(HelHandle handle,
		size_t *size);

HEL_C_LINKAGE HelError helCreateThread(HelHandle address_space,
		HelHandle directory, struct HelThreadState *state, HelHandle *handle);
HEL_C_LINKAGE HelError helExitThisThread();

HEL_C_LINKAGE HelError helCreateEventHub(HelHandle *handle);
HEL_C_LINKAGE HelError helWaitForEvents(HelHandle handle,
		struct HelEvent *list, size_t max_items,
		HelNanotime max_time, size_t *num_items);

HEL_C_LINKAGE HelError helCreateBiDirectionPipe(HelHandle *first,
		HelHandle *second);
HEL_C_LINKAGE HelError helSendString(HelHandle handle,
		const uint8_t *buffer, size_t length,
		int64_t msg_request, int64_t msg_sequence);
HEL_C_LINKAGE HelError helSendDescriptor(HelHandle handle, HelHandle send_handle,
		int64_t msg_request, int64_t msg_sequence);
HEL_C_LINKAGE HelError helSubmitRecvString(HelHandle handle,
		HelHandle hub_handle, uint8_t *buffer, size_t max_length,
		int64_t filter_request, int64_t filter_sequence,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id);
HEL_C_LINKAGE HelError helSubmitRecvDescriptor(HelHandle handle, HelHandle hub_handle,
		int64_t filter_request, int64_t filter_sequence,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id);

HEL_C_LINKAGE HelError helCreateServer(HelHandle *server_handle,
		HelHandle *client_handle);
HEL_C_LINKAGE HelError helSubmitAccept(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id);
HEL_C_LINKAGE HelError helSubmitConnect(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id);

HEL_C_LINKAGE HelError helCreateRd(HelHandle *handle);
HEL_C_LINKAGE HelError helRdMount(HelHandle handle,
		const char *name, size_t name_length,
		HelHandle mount_handle);
HEL_C_LINKAGE HelError helRdPublish(HelHandle handle,
		const char *name, size_t name_length,
		HelHandle publish_handle);
HEL_C_LINKAGE HelError helRdUnlink(HelHandle handle,
		const char *name, size_t name_length);
HEL_C_LINKAGE HelError helRdOpen(const char *path,
		size_t path_length, HelHandle *handle);

HEL_C_LINKAGE HelError helAccessIrq(int number, HelHandle *handle);
HEL_C_LINKAGE HelError helSubmitWaitForIrq(HelHandle handle, HelHandle hub_handle,
		uintptr_t submit_function, uintptr_t submit_object, int64_t *async_id);

HEL_C_LINKAGE HelError helAccessIo(uintptr_t *port_array, size_t num_ports,
		HelHandle *handle);
HEL_C_LINKAGE HelError helEnableIo(HelHandle handle);

