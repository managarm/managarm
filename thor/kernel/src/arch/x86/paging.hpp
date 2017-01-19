
namespace thor {

enum {
	kPageSize = 0x1000,
	kPageShift = 12
};

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
	friend void swap(PageAccessor &a, PageAccessor &b) {
		using frigg::swap;
		swap(a._window, b._window);
		swap(a._pointer, b._pointer);
	}

	PageAccessor()
	: _window{nullptr} { }

	PageAccessor(PhysicalWindow &window, PhysicalAddr physical)
	: _window{&window} {
		_pointer = _window->acquire(physical);
	}

	PageAccessor(const PageAccessor &) = delete;
	
	PageAccessor(PageAccessor &&other)
	: PageAccessor{} {
		swap(*this, other);
	}

	~PageAccessor() {
		if(_window)
			_window->release(_pointer);
	}
	
	PageAccessor &operator= (PageAccessor other) {
		swap(*this, other);
		return *this;
	}

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

	void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical, bool user_access, uint32_t flags);
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

