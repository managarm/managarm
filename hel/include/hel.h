
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
	kHelCallMapMemory = 6,
	
	kHelCallCreateThread = 3,
	kHelCallExitThisThread = 5,
	
	kHelCallCreateEventHub = 13,
	kHelCallWaitForEvents = 16,

	kHelCallCreateBiDirectionPipe = 4,
	kHelCallSendString = 8,
	kHelCallSubmitRecvString = 9,
	
	kHelCallCreateServer = 17,
	kHelCallSubmitAccept = 18,
	kHelCallSubmitConnect = 19,

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
	kHelWaitInfinite = -1
};

enum {
	kHelEventRecvString = 1,
	kHelEventAccept = 2,
	kHelEventConnect = 3,
	kHelEventIrq = 4
};

struct HelEvent {
	int type;
	int error;

	size_t length;
	HelHandle handle;

	uintptr_t submitFunction;
	uintptr_t submitObject;
	int64_t submitId;
};

HEL_C_LINKAGE HelError helLog(const char *string, size_t length);
HEL_C_LINKAGE void helPanic(const char *string, size_t length);

HEL_C_LINKAGE HelError helCloseDescriptor(HelHandle handle);

HEL_C_LINKAGE HelError helAllocateMemory(size_t size, HelHandle *handle);
HEL_C_LINKAGE HelError helMapMemory(HelHandle resource,
		void *pointer, size_t size, void **actual_pointer);

HEL_C_LINKAGE HelError helCreateThread(void (*entry)(uintptr_t argument),
		uintptr_t argument, void *stack_ptr, HelHandle *handle);
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
HEL_C_LINKAGE HelError helSubmitRecvString(HelHandle handle,
		HelHandle hub_handle, uint8_t *buffer, size_t max_length,
		int64_t filter_request, int64_t filter_sequence,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object);

HEL_C_LINKAGE HelError helCreateServer(HelHandle *server_handle,
		HelHandle *client_handle);
HEL_C_LINKAGE HelError helSubmitAccept(HelHandle handle, HelHandle hub_handle,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object);
HEL_C_LINKAGE HelError helSubmitConnect(HelHandle handle, HelHandle hub_handle,
		int64_t submit_id, uintptr_t submit_function, uintptr_t submit_object);

HEL_C_LINKAGE HelError helAccessIrq(int number, HelHandle *handle);
HEL_C_LINKAGE HelError helSubmitWaitForIrq(HelHandle handle,
		HelHandle hub_handle, int64_t submit_id,
		uintptr_t submit_function, uintptr_t submit_object);

HEL_C_LINKAGE HelError helAccessIo(uintptr_t *port_array, size_t num_ports,
		HelHandle *handle);
HEL_C_LINKAGE HelError helEnableIo(HelHandle handle);

