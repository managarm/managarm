
enum {
	kHelCallCreateMemory = 1,
	kHelCallMapMemory = 2
};

typedef uint64_t HelHandle;
typedef HelHandle HelResource;
typedef HelHandle HelDescriptor;

extern "C" HelResource helCreateMemory(size_t length);
extern "C" void helMapMemory(HelResource resource, void *pointer, size_t length);

