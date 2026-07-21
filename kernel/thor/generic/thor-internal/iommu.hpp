#pragma once

#include <frg/vector.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <thor-internal/pci/pci.hpp>
#include <stddef.h>

namespace thor {

namespace pci {

struct PciEntity;

}

struct Iommu {
	Iommu(size_t id)
	: id_{id} {}

	size_t id() const {
		return id_;
	}

	virtual coroutine<void> enableDevice(pci::PciEntity *dev, bool passthrough = true) = 0;

private:
	const size_t id_;
};

// A page space used specifically for DMA.
struct DmaSpace : VirtualSpace {
protected:
	DmaSpace(VirtualOperations *ops)
	: VirtualSpace{ops} { }
};

struct NoopDmaSpace final : DmaSpace {
private:
	struct CtorToken {};

public:
	NoopDmaSpace(CtorToken) : DmaSpace(&ops_) {}

	static std::expected<smarter::shared_ptr<NoopDmaSpace>, Error> create() {
		auto ptr = smarter::allocate_shared<NoopDmaSpace>(*kernelAlloc, CtorToken{});
		ptr->selfPtr = ptr;
		ptr->setupInitialHole(0, 1UL << 39);
		return ptr;
	}

	struct NoopVirtualOperations final : VirtualOperations {
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
	};

private:
	NoopVirtualOperations ops_{};
};

// Represents a collection of devices that share the same DMA space.
struct IommuDomain {
	IommuDomain(smarter::shared_ptr<DmaSpace> space);

	size_t id() const {
		return id_;
	}

	void addMember(pci::PciEntity *e);

	const frg::vector<pci::PciEntity *, KernelAlloc> &members() const {
		return members_;
	}

private:
	size_t id_;
	frg::vector<pci::PciEntity *, KernelAlloc> members_{*kernelAlloc};

public:
	smarter::shared_ptr<DmaSpace> space_;
};

}
