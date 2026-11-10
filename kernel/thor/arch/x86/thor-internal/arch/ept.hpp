#pragma once

#include <atomic>

#include <frg/dyn_array.hpp>

#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/virtualization.hpp>

constexpr uint64_t EPT_READ = (0);
constexpr uint64_t EPT_WRITE = (1);
constexpr uint64_t EPT_EXEC = (2);
constexpr uint64_t EPT_USEREXEC = (10);
constexpr uint64_t EPT_PHYSADDR = (12);
constexpr uint64_t EPT_IGNORE_PAT = (6);
constexpr uint64_t EPT_MEMORY_TYPE = (3);

namespace thor::vmx {

// Whether the CPU supports accessed and dirty flags in EPT entries (IA32_VMX_EPT_VPID_CAP bit 21).
extern bool eptAccessedDirty;

// Returns the EPT pointer (as used by the VMCS and by INVEPT) for the given root table.
uint64_t eptPointer(PhysicalAddr rootTable);

// Invalidates the cached translations of the EPT with the given root table on this CPU.
void inveptSingleContext(PhysicalAddr rootTable);

struct EptPtr {
	uint64_t eptp;
	uint64_t gpa;
};

struct EptPageSpace : PageSpace {
	EptPageSpace(PhysicalAddr root);

	EptPageSpace(const EptPageSpace &) = delete;

	~EptPageSpace();

	EptPageSpace &operator= (const EptPageSpace &) = delete;

	std::atomic<uint64_t> generation{0};

	// The CPU on which a vCPU of this space is in the guest (or about to enter it).
	// Shootdowns wait in shootQueue until it leaves the guest. We only track a single CPU;
	// hence, only one vCPU of a space can be in the guest at a time.
	std::atomic<CpuData *> guestCpu{nullptr};
	// Protects shootQueue; guestCpu is only cleared while this is held.
	frg::ticket_spinlock shootMutex;
	ShootQueue shootQueue;
};

struct EptOperations final : VirtualOperations {
	EptOperations(EptPageSpace *pageSpace);

	void retire(RetireNode *node) override;

	bool submitShootdown(ShootNode *node) override;

	frg::expected<Error, PagesAffected> mapPresentPages(VirtualAddr va, MemoryView *view,
			uintptr_t offset, size_t size, PageFlags flags, CachingMode mode,
			RevokeBatch &batch, PageMagazine &tables) override;

	frg::expected<Error, PagesAffected> restrictPages(VirtualAddr va,
			size_t size, PageFlags flags, RevokeBatch &batch) override;

	frg::expected<Error, PagesAffected> faultPage(VirtualAddr va, MemoryView *view,
			uintptr_t offset, FetchFlags fetchFlags, PageFlags flags, CachingMode mode,
			RevokeBatch &batch, PageMagazine &tables) override;

	frg::expected<Error, PagesAffected> cleanPages(VirtualAddr va, size_t size,
			RevokeBatch &batch) override;

	frg::expected<Error, PagesAffected> unmapPages(VirtualAddr va, size_t size,
			RevokeBatch &batch) override;

	frg::expected<Error, PagesAffected> agePages(VirtualAddr va, size_t size, bool vacate,
			RevokeBatch &batch) override;

private:
	EptPageSpace *pageSpace_;
};

struct EptSpace final : VirtualizedPageSpace {
	friend struct Vmcs;
	friend struct ShootNode;

private:
	struct CtorToken {};

public:
	EptSpace(CtorToken, PhysicalAddr root);

	EptSpace(const EptSpace &) = delete;

	~EptSpace();

	EptSpace& operator=(const EptSpace &) = delete;

	static std::expected<smarter::shared_ptr<EptSpace>, Error> create() {
		PhysicalAddr root = physicalAllocator->allocate(kPageSize);
		if(root == static_cast<PhysicalAddr>(-1))
			return std::unexpected{Error::noMemory};
		PageAccessor accessor{root};
		memset(accessor.get(), 0, kPageSize);

		auto ptr = allocate_rcu_shared<EptSpace>(Allocator{}, CtorToken{}, root);
		ptr->selfPtr = ptr;
		ptr->setupInitialHole(0, 0x7ffffff00000);
		return ptr;
	}

	PhysicalAddr rootTable() {
		return pageSpace_.rootTable();
	}

	// Must be called with interrupts disabled before VM entries on the current CPU.
	// Invalidates the guest-physical translations of this EPT on that CPU unless the EPT
	// did not change since the last invalidation on it. Until leaveGuest(), shootdowns wait
	// for the CPU. Returns false if another vCPU of this space is in the guest.
	bool enterGuest(CpuData *cpu);
	// Must be called with interrupts disabled after VM exits. Completes the shootdowns
	// that waited for the CPU.
	void leaveGuest();

private:
	// For each CPU, the generation (plus one) that it last invalidated (0 means never).
	frg::dyn_array<std::atomic<uint64_t>, KernelAlloc> invalidatedGenerations_;

	EptOperations eptOps_;
	EptPageSpace pageSpace_;
	frg::ticket_spinlock _mutex;
};

} // namespace thor
