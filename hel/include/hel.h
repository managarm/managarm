
enum {
	kHelCallLog = 1,

	kHelCallAllocateMemory = 2,
	kHelCallMapMemory = 6,
	
	kHelCallCreateThread = 3,

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

extern "C" HelError helLog(const char *string, size_t length);

extern "C" HelError helAllocateMemory(size_t size, HelHandle *handle);
extern "C" HelError helMapMemory(HelHandle resource, void *pointer, size_t size);

extern "C" HelError helCreateThread(void (*entry)(uintptr_t argument),
		uintptr_t argument, void *stack_ptr, HelHandle *handle);

extern "C" HelError helCreateBiDirectionPipe(HelHandle *first,
		HelHandle *second);
extern "C" HelError helRecvString(HelHandle handle, char *buffer, size_t length);
extern "C" HelError helSendString(HelHandle handle, const char *buffer, size_t length);

extern "C" void helSwitchThread(HelHandle thread_handle);
