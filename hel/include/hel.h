
enum {
	kHelCallLog = 1,
	kHelCallCreateMemory = 2,
	kHelCallCreateThread = 3,

	kHelCallCreateBiDirectionPipe = 4,
	kHelCallRecvString = 9,
	kHelCallSendString = 8,
	
	kHelCallMapMemory = 6,
	kHelCallSwitchThread = 7
};

enum {
	kHelErrNone = 0
};

typedef int HelError;
typedef uint64_t HelHandle;

extern "C" HelError helLog(const char *string, size_t length);

extern "C" HelError helCreateMemory(size_t length, HelHandle *handle);
extern "C" HelHandle helCreateThread(void *entry);

extern "C" HelError helCreateBiDirectionPipe(HelHandle *first,
		HelHandle *second);
extern "C" HelError helRecvString(HelHandle handle, char *buffer, size_t length);
extern "C" HelError helSendString(HelHandle handle, const char *buffer, size_t length);

extern "C" void helMapMemory(HelHandle resource, void *pointer, size_t length);
extern "C" void helSwitchThread(HelHandle thread_handle);
