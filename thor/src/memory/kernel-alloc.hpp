
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
	struct Header {
		size_t numPages;
		uint8_t padding[32 - sizeof(size_t)];

		Header(size_t num_pages);
	};

	static_assert(sizeof(Header) == 32, "Header is not 32 bytes long");

	void *allocate(size_t length);
	void free(void *pointer);

private:
	StupidVirtualAllocator p_virtualAllocator;
};

}} // namespace thor::memory

