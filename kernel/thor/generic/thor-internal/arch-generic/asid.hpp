#pragma once

#include <smarter.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/types.hpp>
#include <frg/list.hpp>
#include <frg/vector.hpp>
#include <cstddef>
#include <cstdint>

namespace thor {

struct RetireNode {
	friend struct PageSpace;
	friend struct PageBinding;

	virtual void complete() = 0;

protected:
	virtual ~RetireNode() = default;
};

struct ShootNode {
	friend struct PageSpace;
	friend struct PageBinding;

	VirtualAddr address;
	size_t size;

	virtual void complete() = 0;

	frg::default_list_hook<ShootNode> queueNode;

protected:
	virtual ~ShootNode() = default;

private:
	// This CPU already performed synchronous shootdown,
	// hence it can ignore this request during asynchronous shootdown.
	void *initiatorCpu_;

	// Timestamp at which shootdown began.
	uint64_t sequence_;

	std::atomic<size_t> bindingsToShoot_;

};

using ShootNodeList = frg::intrusive_list<
	ShootNode,
	frg::locate_member<
		ShootNode,
		frg::default_list_hook<ShootNode>,
		&ShootNode::queueNode
	>
>;


struct PageBinding;

// Per-CPU context for paging.
struct PageContext {
	friend struct PageBinding;

	PageContext() = default;

	PageContext(const PageContext &) = delete;
	PageContext &operator=(const PageContext &) = delete;

private:
	// Timestamp for the LRU mechansim of ASIDs.
	uint64_t nextStamp_ = 1;

	// Current primary binding (i.e. the currently active ASID).
	PageBinding *primaryBinding_ = nullptr;
};


inline constexpr int globalBindingId = -1;

struct PageSpace;

struct PageBinding {
	friend void swap(PageBinding &a, PageBinding &b) {
		using std::swap;
		swap(a.id_, b.id_);
		swap(a.boundSpace_, b.boundSpace_);
		swap(a.primaryStamp_, b.primaryStamp_);
		swap(a.alreadyShotSequence_, b.alreadyShotSequence_);
	}

	PageBinding() = default;

	PageBinding(const PageBinding &) = delete;
	PageBinding &operator=(const PageBinding &) = delete;


	PageBinding(PageBinding &&other) : PageBinding{} {
		swap(*this, other);
	}

	PageBinding &operator=(PageBinding &&other) {
		swap(*this, other);
		return *this;
	}


	smarter::shared_ptr<PageSpace> boundSpace() {
		return boundSpace_;
	}

	void initialize(int id) {
		assert(!id_);
		id_ = id;
	}

	int id() {
		return id_;
	}

	uint64_t primaryStamp() {
		return primaryStamp_;
	}

	bool isPrimary();

	// Make this binding the primary one on this CPU.
	void rebind();

	// Rebind this binding to a new page space, and make it the
	// primary one on this CPU.
	void rebind(smarter::shared_ptr<PageSpace> space);

	// Unbind from the currently bound space.
	void unbind();

	// Perform any pending shootdowns for the currently bound
	// space.
	void shootdown();

private:
	ShootNodeList completeShootdown_(PageSpace *space, uint64_t afterSequence,
			bool doShootdown);

	int id_ = 0;

	// TODO: Once we can use libsmarter in the kernel, we should make this a shared_ptr
	//       to the PageSpace that does *not* prevent the PageSpace from becoming
	//       "activatable".
	smarter::shared_ptr<PageSpace> boundSpace_ = nullptr;

	uint64_t primaryStamp_ = 0;

	uint64_t alreadyShotSequence_ = 0;
};


struct PageSpace {
	friend struct PageBinding;

	// Switch to the given page space on this CPU. Picks the least
	// recently used binding to use for the switch.
	static void activate(smarter::shared_ptr<PageSpace> space);

	PageSpace(PhysicalAddr rootTable);

	~PageSpace();

	PhysicalAddr rootTable() {
		return rootTable_;
	}

	// Initiat asynchronous retirement this page space. Waits for
	// all bindings to unbind from it before completing.
	void retire(RetireNode *node);
	// Initiate an asynchronous TLB shootdown for a range of pages
	// within this page space. Waits for all CPUs to perform the
	// shootdown.
	bool submitShootdown(ShootNode *node);

	auto &tableMutex() {
		return tableMutex_;
	}

private:
	PhysicalAddr rootTable_;

	std::atomic<bool> wantToRetire_ = false;
	RetireNode *retireNode_ = nullptr;

	frg::ticket_spinlock mutex_;
	frg::ticket_spinlock tableMutex_;

	unsigned int numBindings_;

	uint64_t shootSequence_;

	ShootNodeList shootQueue_;
};


struct AsidCpuData {
	AsidCpuData(size_t maxBindings)
	: globalBinding{}, bindings{*kernelAlloc} {
		bindings.resize(maxBindings);
		for(size_t i = 0; i < maxBindings; i++) {
			bindings[i].initialize(i);
		}
	}

	PageContext pageContext;
	PageBinding globalBinding;
	frg::vector<PageBinding, KernelAlloc> bindings;
};


// Switch to given page table on the given ASID. Potentially
// invalidate the TLB entries for the ASID that's being used.
void switchToPageTable(PhysicalAddr root, int asid, bool invalidate);

// Switch away from the current user page tables to kernel-only page tables.
// Also invalidate the given ASID.
// This is called when the currently active page tables need to be destroyed.
void switchAwayFromPageTable(int asid);

// Invalidate the TLB entries for the given ASID.
// (globalBindingId for the global page tables).
void invalidateAsid(int asid);

// Invalidate the page at the given address within the given ASID
// (globalBindingId for the global page tables).
void invalidatePage(int asid, const void *address);


struct CpuData;

// Initialize the ASID context on the given CPU.
void initializeAsidContext(CpuData *cpuData);

} // namespace thor
