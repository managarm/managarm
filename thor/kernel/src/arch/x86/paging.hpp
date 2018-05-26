#ifndef THOR_ARCH_X86_PAGING_HPP
#define THOR_ARCH_X86_PAGING_HPP

#include <atomic>
#include <frg/list.hpp>
#include <frigg/algorithm.hpp>
#include "../../generic/types.hpp"

namespace thor {

void initializePhysicalAccess();

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

void sendShootdownIpi();

struct ShootNode {
	friend struct PageSpace;
	friend struct PageBinding;

	VirtualAddr address;
	size_t size;

	void (*shotDown)(ShootNode *node);

private:
	uint64_t _sequence;

	std::atomic<unsigned int> _bindingsToShoot;

	frg::default_list_hook<ShootNode> _queueNode;
};

struct PageSpace;

struct PageBinding {
	PageBinding();

	PageBinding(const PageBinding &) = delete;
	
	PageBinding &operator= (const PageBinding &) = delete;

	void rebind(PageSpace *space, PhysicalAddr pml4);

	void shootdown();

private:
	PageSpace *_boundSpace;

	uint64_t _alreadyShotSequence;
};

struct PageSpace {
	friend struct PageBinding;

	PageSpace();

	void submitShootdown(ShootNode *node);

private:
	frigg::TicketLock _mutex;
	
	unsigned int _numBindings;

	uint64_t _shootSequence;

	frg::intrusive_list<
		ShootNode,
		frg::locate_member<
			ShootNode,
			frg::default_list_hook<ShootNode>,
			&ShootNode::_queueNode
		>
	> _shootQueue;
};

namespace page_mode {
	static constexpr uint32_t remap = 1;
}

enum class PageMode {
	null,
	normal,
	remap
};

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

	PhysicalAddr getPml4();

private:
	PhysicalAddr _pml4Address;
};

struct ClientPageSpace : PageSpace {
public:
	ClientPageSpace();
	
	ClientPageSpace(const ClientPageSpace &) = delete;
	
	~ClientPageSpace();

	ClientPageSpace &operator= (const ClientPageSpace &) = delete;
	
	void activate();

	void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical, bool user_access, uint32_t flags);
	void unmapRange(VirtualAddr pointer, size_t size, PageMode mode);
	bool isMapped(VirtualAddr pointer);

private:
	PhysicalAddr _pml4Address;
};

void invalidatePage(const void *address);

} // namespace thor

#endif // THOR_ARCH_X86_PAGING_HPP
