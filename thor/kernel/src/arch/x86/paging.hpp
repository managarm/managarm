
namespace thor {

enum {
	kPageSize = 0x1000,
	kPageShift = 12
};

void *physicalToVirtual(PhysicalAddr address);

template<typename T>
T *accessPhysical(PhysicalAddr address) {
	return (T *)physicalToVirtual(address);
}

template<typename T>
T *accessPhysicalN(PhysicalAddr address, int n) {
	(void)n;

	return (T *)physicalToVirtual(address);
}

struct PhysicalWindow {
	constexpr PhysicalWindow(uint64_t *table, void *content);

	void *acquire(PhysicalAddr physical);
	void release(void *pointer);

private:
	uint64_t *_table;
	void *_content;
	bool _locked[512];
};

struct PageAccessor {
	PageAccessor(PhysicalWindow &window, PhysicalAddr physical)
	: _window(&window) {
		_pointer = _window->acquire(physical);
	}

	PageAccessor(const PageAccessor &) = delete;

	~PageAccessor() {
		_window->release(_pointer);
	}
	
	PageAccessor &operator= (const PageAccessor &) = delete;

	void *get() {
		return _pointer;
	}

private:
	PhysicalWindow *_window;
	void *_pointer;
};

extern PhysicalWindow generalWindow;

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

