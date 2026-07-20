#pragma once

#include <expected>

#include <thor-internal/arch-generic/paging.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/physical.hpp>
#include <thor-internal/virtualization.hpp>

namespace thor::riscv_hypervisor {

struct HypervisorPageSpace : PageSpace {
	HypervisorPageSpace(PhysicalAddr root);

	HypervisorPageSpace(const HypervisorPageSpace &) = delete;
	HypervisorPageSpace &operator=(const HypervisorPageSpace &) = delete;

	~HypervisorPageSpace();
};

struct HypervisorSpace final : VirtualizedPageSpace {
private:
	struct CtorToken {};

public:
	struct Operations final : VirtualOperations {
		Operations(HypervisorPageSpace *pageSpace);

		void retire(RetireNode *node) override;

		bool submitShootdown(ShootNode *node) override;

		frg::expected<Error, PagesAffected> mapPresentPages(
		    VirtualAddr va,
		    MemoryView *view,
		    uintptr_t offset,
		    size_t size,
		    PageFlags flags,
		    CachingMode mode
		) override;

		frg::expected<Error, PagesAffected>
		restrictPages(VirtualAddr va, size_t size, PageFlags flags, CachingMode mode) override;

		frg::expected<Error, PagesAffected> faultPage(
		    VirtualAddr va,
		    MemoryView *view,
		    uintptr_t offset,
		    FetchFlags fetchFlags,
		    PageFlags flags,
		    CachingMode mode
		) override;

		frg::expected<Error, PagesAffected> cleanPages(VirtualAddr va, size_t size) override;

		frg::expected<Error, PagesAffected> unmapPages(VirtualAddr va, size_t size) override;

		frg::expected<Error, PagesAffected>
		agePages(VirtualAddr va, size_t size, bool vacate) override;

	private:
		HypervisorPageSpace *pageSpace_;
	};

	HypervisorSpace(CtorToken, PhysicalAddr root);

	HypervisorSpace(const HypervisorSpace &) = delete;
	HypervisorSpace &operator=(const HypervisorSpace &) = delete;

	static std::expected<smarter::shared_ptr<HypervisorSpace>, Error> create() {
		PhysicalAddr root = physicalAllocator->allocate(0x4000);
		if (root == static_cast<PhysicalAddr>(-1))
			return std::unexpected{Error::noMemory};
		PageAccessor accessor{root};
		memset(accessor.get(), 0, 0x4000);

		auto ptr = smarter::allocate_shared<HypervisorSpace>(Allocator{}, CtorToken{}, root);
		ptr->selfPtr = ptr;
		// TODO: This could be bigger on sv48/sv57 or when taking advantage of the bigger level 0
		// table, it's unlikely to be an issue in practice though.
		ptr->setupInitialHole(0, 0x8000000000);
		return ptr;
	}

	PhysicalAddr rootTable() { return pageSpace_.rootTable(); }

private:
	HypervisorPageSpace pageSpace_;
	Operations ops_;
};

} // namespace thor::riscv_hypervisor
