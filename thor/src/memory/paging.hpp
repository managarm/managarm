
namespace thor {
namespace memory {

void *physicalToVirtual(uintptr_t address);

enum PageFlags {
	kPageSize = 0x1000,
	kPagePresent = 0x1,
	kPageWrite = 0x2,
	kPageUser = 0x4
};

class PageSpace {
public:
	PageSpace(uintptr_t pml4_address);
	
	void switchTo();

	PageSpace clone();

	void mapSingle4k(void *pointer, uintptr_t physical);
	void unmapSingle4k(VirtualAddr pointer);

private:
	uintptr_t p_pml4Address;
};

extern LazyInitializer<PageSpace> kernelSpace;

}} // namespace thor::memory

