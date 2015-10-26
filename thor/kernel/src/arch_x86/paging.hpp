
namespace thor {

void *physicalToVirtual(PhysicalAddr address);

template<typename T>
T *accessPhysical(PhysicalAddr address) {
	return (T *)physicalToVirtual(address);
}

template<typename T>
T *accessPhysicalN(PhysicalAddr address, int n) {
	return (T *)physicalToVirtual(address);
}

enum {
	kPageSize = 0x1000
};

class PageSpace {
public:
	enum : uint32_t {
		kAccessWrite = 1,
		kAccessExecute = 2
	};

	PageSpace(PhysicalAddr pml4_address);
	
	void activate();

	PageSpace cloneFromKernelSpace();

	void mapSingle4k(PhysicalChunkAllocator::Guard &physical_guard,
			VirtualAddr pointer, PhysicalAddr physical, bool user_access, uint32_t flags);
	PhysicalAddr unmapSingle4k(VirtualAddr pointer);
	bool isMapped(VirtualAddr pointer);

	PhysicalAddr getPml4();

private:
	PhysicalAddr p_pml4Address;
};

extern frigg::LazyInitializer<PageSpace> kernelSpace;

extern "C" void thorRtInvalidatePage(void *pointer);
extern "C" void thorRtInvalidateSpace();

} // namespace thor

