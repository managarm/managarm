
#ifdef __cplusplus
#define HEL_C_LINKAGE extern "C"
#else
#define HEL_C_LINKAGE
#endif

enum {
	kHelCallLog = 1,
	kHelCallPanic = 10,

	kHelCallAllocateMemory = 2,
	kHelCallMapMemory = 6,
	
	kHelCallCreateThread = 3,
	kHelCallExitThisThread = 5,

	kHelCallCreateBiDirectionPipe = 4,
	kHelCallRecvString = 9,
	kHelCallSendString = 8,
	
	kHelCallSwitchThread = 7
};

enum {
	kHelErrNone = 0
};

typedef int HelError;
typedef uint64_t HelHandle;

HEL_C_LINKAGE HelError helLog(const char *string, size_t length);
HEL_C_LINKAGE void helPanic(const char *string, size_t length);

HEL_C_LINKAGE HelError helAllocateMemory(size_t size, HelHandle *handle);
HEL_C_LINKAGE HelError helMapMemory(HelHandle resource, void *pointer, size_t size);

HEL_C_LINKAGE HelError helCreateThread(void (*entry)(uintptr_t argument),
		uintptr_t argument, void *stack_ptr, HelHandle *handle);

HEL_C_LINKAGE HelError helCreateBiDirectionPipe(HelHandle *first,
		HelHandle *second);
HEL_C_LINKAGE HelError helRecvString(HelHandle handle, char *buffer, size_t length);
HEL_C_LINKAGE HelError helSendString(HelHandle handle, const char *buffer, size_t length);

HEL_C_LINKAGE void helSwitchThread(HelHandle thread_handle);
