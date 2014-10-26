
enum {
	kHelCallCreateMemory = 1,
	kHelCallCreateThread = 2,
	kHelCallMapMemory = 3,
	kHelCallSwitchThread = 4
};

typedef uint64_t HelDescriptor;

extern "C" HelDescriptor helCreateMemory(size_t length);
extern "C" void helMapMemory(HelDescriptor resource, void *pointer, size_t length);
extern "C" HelDescriptor helCreateThread(void *entry);
extern "C" void helSwitchThread(HelDescriptor thread_handle);

