
enum {
	kHelCallCreateMemory = 1,
	kHelCallCreateThread = 2,
	kHelCallMapMemory = 3,
	kHelCallSwitchThread = 4
};

typedef uint64_t HelHandle;
typedef HelHandle HelResource;
typedef HelHandle HelDescriptor;

extern "C" HelResource helCreateMemory(size_t length);
extern "C" void helMapMemory(HelResource resource, void *pointer, size_t length);
extern "C" HelResource helCreateThread();
extern "C" void helSwitchThread(HelResource thread_handle);

