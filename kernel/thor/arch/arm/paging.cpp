#include <thor-internal/arch/paging.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/core.hpp>

namespace thor {

void poisonPhysicalAccess(PhysicalAddr physical) { assert(!"Not implemented"); }
void poisonPhysicalWriteAccess(PhysicalAddr physical) { assert(!"Not implemented"); }

PageContext::PageContext() { }

PageBinding::PageBinding() { assert(!"Not implemented"); }
bool PageBinding::isPrimary() { assert(!"Not implemented"); return false; }
void PageBinding::rebind() { assert(!"Not implemented"); }
void PageBinding::rebind(smarter::shared_ptr<PageSpace> space) { assert(!"Not implemented"); }
void PageBinding::unbind() { assert(!"Not implemented"); }
void PageBinding::shootdown() { assert(!"Not implemented"); }

GlobalPageBinding::GlobalPageBinding()
: _alreadyShotSequence{0} { }

void GlobalPageBinding::bind() {
	assert(!intsAreEnabled());

	auto space = &KernelPageSpace::global();

	uint64_t targetSeq;
	{
		auto lock = frg::guard(&space->_shootMutex);

		targetSeq = space->_shootSequence;
		space->_numBindings++;
	}

	_alreadyShotSequence = targetSeq;
}

void GlobalPageBinding::shootdown() { assert(!"Not implemented"); }

void PageSpace::activate(smarter::shared_ptr<PageSpace> space) { assert(!"Not implemented"); }
PageSpace::PageSpace(PhysicalAddr root_table) { assert(!"Not implemented"); }
PageSpace::~PageSpace() { assert(!"Not implemented"); }
void PageSpace::retire(RetireNode *node) { assert(!"Not implemented"); }
bool PageSpace::submitShootdown(ShootNode *node) { assert(!"Not implemented"); return false; }

frg::manual_box<KernelPageSpace> kernelSpaceSingleton;

void KernelPageSpace::initialize() {
	PhysicalAddr ttbr1_ptr;
	asm volatile ("mrs %0, ttbr1_el1" : "=r" (ttbr1_ptr));

	kernelSpaceSingleton.initialize(ttbr1_ptr);
}

KernelPageSpace &KernelPageSpace::global() {
	return *kernelSpaceSingleton;
}

KernelPageSpace::KernelPageSpace(PhysicalAddr ttbr1_ptr)
:ttbr1_{ttbr1_ptr} { }

bool KernelPageSpace::submitShootdown(ShootNode *node) { assert(!"Not implemented"); return false; }

static inline constexpr uint64_t kPageValid = 1;
static inline constexpr uint64_t kPageTable = (1 << 1);
static inline constexpr uint64_t kPageL3Page = (1 << 1);
static inline constexpr uint64_t kPageXN = (uint64_t(1) << 54);
static inline constexpr uint64_t kPagePXN = (uint64_t(1) << 53);
static inline constexpr uint64_t kPageNotGlobal = (1 << 11);
static inline constexpr uint64_t kPageAccess = (1 << 10);
static inline constexpr uint64_t kPageRO = (1 << 7);
static inline constexpr uint64_t kPageInnerSh = (3 << 8);
static inline constexpr uint64_t kPageOuterSh = (2 << 8);
static inline constexpr uint64_t kPageWb = (0 << 2);
static inline constexpr uint64_t kPageGRE = (1 << 2);

void KernelPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical,
		uint32_t flags, CachingMode caching_mode) {
	assert((pointer % 0x1000) == 0);
	assert((physical % 0x1000) == 0);

	auto ttbr = (pointer >> 63) & 1;
	assert(ttbr == 1);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto &region = SkeletalRegion::global();

	auto l0 = (pointer >> 39) & 0x1FF;
	auto l1 = (pointer >> 30) & 0x1FF;
	auto l2 = (pointer >> 21) & 0x1FF;
	auto l3 = (pointer >> 12) & 0x1FF;

	uint64_t *l0_ptr = (uint64_t *)region.access(rootTable() & 0xFFFFFFFFFFFE);
	uint64_t *l1_ptr = nullptr;
	uint64_t *l2_ptr = nullptr;
	uint64_t *l3_ptr = nullptr;

	auto l0_ent = l0_ptr[l0];
	if (!(l0_ent & kPageValid)) {
		PhysicalAddr page = physicalAllocator->allocate(kPageSize);
		assert(page != static_cast<PhysicalAddr>(-1) && "OOM");

		l1_ptr = (uint64_t *)region.access(page);

		for(int i = 0; i < 512; i++)
			l1_ptr[i] = 0;

		l0_ptr[l0] =
			page | kPageValid | kPageTable;
	} else {
		l1_ptr = (uint64_t *)region.access(l0_ent & 0xFFFFFFFFF000);
	}

	auto l1_ent = l1_ptr[l1];
	if (!(l1_ent & kPageValid)) {
		PhysicalAddr page = physicalAllocator->allocate(kPageSize);
		assert(page != static_cast<PhysicalAddr>(-1) && "OOM");

		l2_ptr = (uint64_t *)region.access(page);

		for(int i = 0; i < 512; i++)
			l2_ptr[i] = 0;

		l1_ptr[l1] =
			page | kPageValid | kPageTable;
	} else {
		l2_ptr = (uint64_t *)region.access(l1_ent & 0xFFFFFFFFF000);
	}

	auto l2_ent = l2_ptr[l2];
	if (!(l2_ent & kPageValid)) {
		PhysicalAddr page = physicalAllocator->allocate(kPageSize);
		assert(page != static_cast<PhysicalAddr>(-1) && "OOM");

		l3_ptr = (uint64_t *)region.access(page);

		for(int i = 0; i < 512; i++)
			l3_ptr[i] = 0;

		l2_ptr[l2] =
			page | kPageValid | kPageTable;
	} else {
		l3_ptr = (uint64_t *)region.access(l2_ent & 0xFFFFFFFFF000);
	}

	auto l3_ent = l3_ptr[l3];

	assert(!(l3_ent & kPageValid));

	uint64_t new_entry = physical | kPageValid | kPageL3Page | kPageAccess;

	if (!(flags & page_access::write))
		new_entry |= kPageRO;
	if (!(flags & page_access::execute))
		new_entry |= kPageXN | kPagePXN;

	if (caching_mode == CachingMode::writeCombine)
		new_entry |= kPageGRE | kPageOuterSh;
	else {
		assert(caching_mode == CachingMode::null || caching_mode == CachingMode::writeBack);
		new_entry |= kPageWb | kPageInnerSh;
	}

	assert(!(new_entry & (uintptr_t(0b111) << 48)));

	l3_ptr[l3] = new_entry;
}

PhysicalAddr KernelPageSpace::unmapSingle4k(VirtualAddr pointer) {
	assert((pointer % 0x1000) == 0);

	auto ttbr = (pointer >> 63) & 1;
	assert(ttbr == 1);

	auto irq_lock = frg::guard(&irqMutex());
	auto lock = frg::guard(&_mutex);

	auto &region = SkeletalRegion::global();

	auto l0 = (pointer >> 39) & 0x1FF;
	auto l1 = (pointer >> 30) & 0x1FF;
	auto l2 = (pointer >> 21) & 0x1FF;
	auto l3 = (pointer >> 12) & 0x1FF;

	uint64_t *l0_ptr = (uint64_t *)region.access(rootTable() & 0xFFFFFFFFFFFE);
	auto l0_ent = l0_ptr[l0];
	assert(l0_ent & kPageValid);

	uint64_t *l1_ptr = (uint64_t *)region.access(l0_ent & 0xFFFFFFFFF000);
	auto l1_ent = l1_ptr[l1];
	assert(l1_ent & kPageValid);

	uint64_t *l2_ptr = (uint64_t *)region.access(l1_ent & 0xFFFFFFFFF000);
	auto l2_ent = l2_ptr[l2];
	assert(l2_ent & kPageValid);

	uint64_t *l3_ptr = (uint64_t *)region.access(l2_ent & 0xFFFFFFFFF000);
	auto l3_ent = l3_ptr[l3];
	assert(l3_ent & kPageValid);

	l3_ptr[l3] ^= kPageValid;

	return l3_ent & 0xFFFFFFFFF000;
}

ClientPageSpace::Walk::Walk(ClientPageSpace *space) { assert(!"Not implemented"); }
ClientPageSpace::Walk::~Walk() { assert(!"Not implemented"); }
void ClientPageSpace::Walk::walkTo(uintptr_t address) { assert(!"Not implemented"); }
PageFlags ClientPageSpace::Walk::peekFlags() { assert(!"Not implemented"); }
PhysicalAddr ClientPageSpace::Walk::peekPhysical() { assert(!"Not implemented"); }

ClientPageSpace::ClientPageSpace() :PageSpace{0} {}
ClientPageSpace::~ClientPageSpace() { assert(!"Not implemented"); }

void ClientPageSpace::mapSingle4k(VirtualAddr pointer, PhysicalAddr physical, bool user_access,
		uint32_t flags, CachingMode caching_mode) { assert(!"Not implemented"); }
PageStatus ClientPageSpace::unmapSingle4k(VirtualAddr pointer) { assert(!"Not implemented"); }
PageStatus ClientPageSpace::cleanSingle4k(VirtualAddr pointer) { assert(!"Not implemented"); }
void ClientPageSpace::unmapRange(VirtualAddr pointer, size_t size, PageMode mode) { assert(!"Not implemented"); }
bool ClientPageSpace::isMapped(VirtualAddr pointer) { assert(!"Not implemented"); }

}
