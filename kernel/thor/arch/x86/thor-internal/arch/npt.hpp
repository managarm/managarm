#pragma once

#include <atomic>

#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/rcu.hpp>
#include <thor-internal/virtualization.hpp>

namespace thor::svm {

struct NptPageSpace : PageSpace {
	NptPageSpace(PhysicalAddr root);

	NptPageSpace(const NptPageSpace &) = delete;

	~NptPageSpace();

	NptPageSpace &operator= (const NptPageSpace &) = delete;

	// The CPU on which a vCPU of this space is in the guest (or about to enter it).
	// Shootdowns wait in shootQueue until it leaves the guest. We only track a single CPU;
	// hence, only one vCPU of a space can be in the guest at a time.
	std::atomic<CpuData *> guestCpu{nullptr};
	// Protects shootQueue; guestCpu is only cleared while this is held.
	frg::ticket_spinlock shootMutex;
	ShootQueue shootQueue;
};

struct NptOperations final : VirtualOperations {
	NptOperations(NptPageSpace *pageSpace);

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
	NptPageSpace *pageSpace_;
};

} // namespace thor::svm

namespace thor::svm {
	struct NptSpace final : VirtualizedPageSpace {
		friend struct ShootNode;
		friend struct Vcpu;

	private:
		struct CtorToken {};

	public:
		NptSpace(CtorToken, PhysicalAddr root);

		NptSpace(const NptSpace &) = delete;

		~NptSpace();

		NptSpace& operator=(const NptSpace &) = delete;

		static std::expected<smarter::shared_ptr<NptSpace>, Error> create() {
			PhysicalAddr root = physicalAllocator->allocate(kPageSize);
			if(root == static_cast<PhysicalAddr>(-1))
				return std::unexpected{Error::noMemory};
			PageAccessor accessor{root};
			memset(accessor.get(), 0, kPageSize);

			auto ptr = allocate_rcu_shared<NptSpace>(Allocator{}, CtorToken{}, root);
			ptr->selfPtr = ptr;
			ptr->setupInitialHole(0, 0x7ffffff00000);
			return ptr;
		}

		PhysicalAddr rootTable() {
			return pageSpace_.rootTable();
		}

		// Must be called with interrupts disabled before VMRUN on the current CPU.
		// Until leaveGuest(), shootdowns wait for the CPU.
		// Returns false if another vCPU of this space is in the guest.
		bool enterGuest(CpuData *cpu);
		// Must be called with interrupts disabled after VM exits. Completes the shootdowns
		// that waited for the CPU.
		void leaveGuest();

	private:
		NptOperations nptOps_;
		NptPageSpace pageSpace_;
		frg::ticket_spinlock _mutex;
	};
} // namespace thor::svm
