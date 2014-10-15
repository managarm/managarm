
namespace thor {
namespace memory {

class StupidVirtualAllocator {
public:
	StupidVirtualAllocator();

	void *allocate(size_t length);

private:
	char *p_nextPointer;
};

class StupidMemoryAllocator {
public:
	void *allocate(size_t length);

private:
	StupidVirtualAllocator p_virtualAllocator;
};

extern LazyInitializer<StupidMemoryAllocator> kernelAllocator;

}} // namespace thor::memory

void *operator new(size_t length, thor::memory::StupidMemoryAllocator *);

