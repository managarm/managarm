
namespace thor {
namespace memory {

void *physicalToVirtual(uintptr_t address);

enum PageFlags {
	kPageSize = 0x1000,
	kPagePresent = 1,
	kPageWrite = 2
};

class PageSpace {
public:
	PageSpace(uintptr_t pml4_address);

	void mapSingle4k(void *pointer, uintptr_t physical);

private:
	uintptr_t p_pml4Address;
};

extern LazyInitializer<PageSpace> kernelSpace;

}} // namespace thor::memory

