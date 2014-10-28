
enum {
	kHelCallLog = 1,
	kHelCallCreateMemory = 2,
	kHelCallCreateThread = 3,
	kHelCallCreateChannel = 4,
	kHelCallMapMemory = 5,
	kHelCallSwitchThread = 6,
	kHelCallSend = 7,
	kHelCallReceive = 8,
	kHelCallRespond = 9
};

typedef uint64_t HelDescriptor;

extern "C" void helLog(const char *string, size_t length);

extern "C" HelDescriptor helCreateMemory(size_t length);
extern "C" HelDescriptor helCreateThread(void *entry);
extern "C" HelDescriptor helCreateChannel();

extern "C" void helMapMemory(HelDescriptor resource, void *pointer, size_t length);
extern "C" void helSwitchThread(HelDescriptor thread_handle);
extern "C" void helSend(HelDescriptor channel_handle);
extern "C" void helReceive(HelDescriptor channel_handle);
extern "C" void helRespond(HelDescriptor channel_handle);
