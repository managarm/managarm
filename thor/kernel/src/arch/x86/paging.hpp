
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

namespace page_access {
	static constexpr uint32_t write = 1;
	static constexpr uint32_t execute = 2;
}

struct KernelPageSpace {
public:
	static void initialize(PhysicalAddr pml4_address);

	static KernelPageSpace &global();

	// TODO: This should be private.
	explicit KernelPageSpace(PhysicalAddr pml4_address);
	
	KernelPageSpace(const KernelPageSpace &) = delete;
	
	KernelPageSpace &operator= (const KernelPageSpace &) = delete;

	void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical, uint32_t flags);
	PhysicalAddr unmapSingle4k(VirtualAddr pointer);
	bool isMapped(VirtualAddr pointer);

	PhysicalAddr getPml4();

private:
	PhysicalAddr _pml4Address;
};

struct ClientPageSpace {
public:
	ClientPageSpace();
	
	ClientPageSpace(const ClientPageSpace &) = delete;
	
	~ClientPageSpace();

	ClientPageSpace &operator= (const ClientPageSpace &) = delete;
	
	void activate();

	void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical, bool user_access, uint32_t flags);
	PhysicalAddr unmapSingle4k(VirtualAddr pointer);
	bool isMapped(VirtualAddr pointer);

	PhysicalAddr getPml4();

private:
	PhysicalAddr _pml4Address;
};

extern "C" void thorRtInvalidatePage(void *pointer);
extern "C" void thorRtInvalidateSpace();

} // namespace thor

