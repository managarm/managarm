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

struct PageAccessor {
	friend void swap(PageAccessor &a, PageAccessor &b) {
		using frigg::swap;
		swap(a._pointer, b._pointer);
	}

	PageAccessor()
	: _pointer{nullptr} { }

	PageAccessor(PhysicalAddr physical) {
		assert(!(physical & (kPageSize - 1)));
		assert(physical < 0x4000'0000'0000);
		_pointer = reinterpret_cast<void *>(0xFFFF'8000'0000'0000 + physical);
	}

	PageAccessor(const PageAccessor &) = delete;
	
	PageAccessor(PageAccessor &&other)
	: PageAccessor{} {
		swap(*this, other);
	}

	~PageAccessor() { }
	
	PageAccessor &operator= (PageAccessor other) {
		swap(*this, other);
		return *this;
	}

	void *get() {
		return _pointer;
	}

private:
	void *_pointer;
};

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
struct PageBinding;

static constexpr int maxPcidCount = 8;

// Per-CPU context for paging.
struct PageContext {
	friend struct PageBinding;

	PageContext();

	PageContext(const PageContext &) = delete;
	
	PageContext &operator= (const PageContext &) = delete;

private:
	// Timestamp for the LRU mechansim of PCIDs.
	uint64_t _nextStamp;

	// Current primary binding (i.e. the currently active PCID).
	PageBinding *_primaryBinding;
};

struct PageBinding {
	PageBinding(int pcid);

	PageBinding(const PageBinding &) = delete;
	
	PageBinding &operator= (const PageBinding &) = delete;

	PageSpace *boundSpace() {
		return _boundSpace;
	}

	uint64_t primaryStamp() {
		return _primaryStamp;
	}

	void makePrimary();

	void rebind(PageSpace *space);

	void shootdown();

private:
	int _pcid;

	PageSpace *_boundSpace;

	bool _wasRebound;

	uint64_t _primaryStamp;

	uint64_t _alreadyShotSequence;
};

struct PageSpace {
	friend struct PageBinding;

	PageSpace(PhysicalAddr root_table);

	PhysicalAddr rootTable() {
		return _rootTable;
	}

	void submitShootdown(ShootNode *node);

private:
	PhysicalAddr _rootTable;

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

struct KernelPageSpace : PageSpace {
public:
	static void initialize(PhysicalAddr pml4_address);

	static KernelPageSpace &global();

	// TODO: This should be private.
	explicit KernelPageSpace(PhysicalAddr pml4_address);
	
	KernelPageSpace(const KernelPageSpace &) = delete;
	
	KernelPageSpace &operator= (const KernelPageSpace &) = delete;

	void mapSingle4k(VirtualAddr pointer, PhysicalAddr physical, uint32_t flags);
	PhysicalAddr unmapSingle4k(VirtualAddr pointer);
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
};

void invalidatePage(const void *address);

} // namespace thor

#endif // THOR_ARCH_X86_PAGING_HPP
