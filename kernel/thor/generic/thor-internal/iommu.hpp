#pragma once

#include <frg/manual_box.hpp>
#include <frg/vector.hpp>
#include <thor-internal/address-space.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/kernel-heap.hpp>
#include <stddef.h>

namespace thor {

// Firmware description that an IOMMU was discovered from. Also determines the meaning of the
// register base that identifies the unit.
enum class IommuKind {
	intelVtd,
	amdVi,
};

// Identifies the requester that a device issues DMA requests as.
struct SourceId {
	uint16_t segment;
	uint8_t bus;
	uint8_t slot;
	uint8_t function;
};

// Range of physical memory that a DMA space identity-maps.
struct DmaReservedRegion {
	uint64_t base;
	uint64_t size;
	bool readable;
	bool writable;
};

// A page space used specifically for DMA.
struct DmaSpace : VirtualSpace, RcuProtected {
protected:
	DmaSpace(VirtualOperations *ops)
	: VirtualSpace{ops} { }
};

struct Iommu {
	Iommu(IommuKind kind, uint64_t registerBase, size_t id)
	: kind_{kind}, registerBase_{registerBase}, id_{id} {}

	IommuKind kind() const {
		return kind_;
	}

	uint64_t registerBase() const {
		return registerBase_;
	}

	size_t id() const {
		return id_;
	}

	// Attaches a device to a DMA space. A null space attaches the device in passthrough mode.
	virtual coroutine<std::expected<void, Error>> attachDevice(SourceId source, DmaSpace *space) = 0;

private:
	const IommuKind kind_;
	const uint64_t registerBase_;
	const size_t id_;
};

// Registry of the IOMMUs that the kernel discovered from firmware. It is populated during boot
// and read-only afterwards.
void registerIommu(smarter::shared_ptr<Iommu> iommu);
smarter::shared_ptr<Iommu> lookupIommu(IommuKind kind, uint64_t registerBase);

struct NoopDmaSpace final : DmaSpace {
private:
	struct CtorToken {};

public:
	NoopDmaSpace(CtorToken) : DmaSpace(&ops_) {}

	static std::expected<smarter::shared_ptr<NoopDmaSpace>, Error> create() {
		auto ptr = allocate_rcu_shared<NoopDmaSpace>(*kernelAlloc, CtorToken{});
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
		restrictPages(VirtualAddr va, size_t size, PageFlags flags) override;

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

}
