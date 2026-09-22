#include <frg/mutex.hpp>
#include <thor-internal/iommu.hpp>

namespace thor {

namespace {

// Registration runs from the firmware discovery tasks while lookups run from syscalls.
constinit IrqSpinlock allIommusMutex;
constinit frg::manual_box<frg::vector<smarter::shared_ptr<Iommu>, KernelAlloc>> allIommus;

} // namespace

void registerIommu(smarter::shared_ptr<Iommu> iommu) {
	auto lock = frg::guard(&allIommusMutex);

	if(!allIommus)
		allIommus.initialize(*kernelAlloc);
	allIommus->push_back(std::move(iommu));
}

smarter::shared_ptr<Iommu> lookupIommu(IommuKind kind, uint64_t registerBase) {
	auto lock = frg::guard(&allIommusMutex);

	if(!allIommus)
		return nullptr;
	for(auto &iommu : *allIommus)
		if(iommu->kind() == kind && iommu->registerBase() == registerBase)
			return iommu;
	return nullptr;
}

void NoopDmaSpace::NoopVirtualOperations::retire(RetireNode *) {}

bool NoopDmaSpace::NoopVirtualOperations::submitShootdown(ShootNode *) { return false; }

frg::expected<Error, PagesAffected> NoopDmaSpace::NoopVirtualOperations::mapPresentPages(
    VirtualAddr, MemoryView *, uintptr_t, size_t, PageFlags, CachingMode, bool
) {
	return PagesAffected{};
}

frg::expected<Error, PagesAffected>
NoopDmaSpace::NoopVirtualOperations::restrictPages(VirtualAddr, size_t, PageFlags, bool) {
	return PagesAffected{};
}

frg::expected<Error, PagesAffected> NoopDmaSpace::NoopVirtualOperations::faultPage(
    VirtualAddr, MemoryView *, uintptr_t, FetchFlags, PageFlags, CachingMode, bool
) {
	return PagesAffected{};
}

frg::expected<Error, PagesAffected>
NoopDmaSpace::NoopVirtualOperations::cleanPages(VirtualAddr, size_t, bool) {
	return PagesAffected{};
}

frg::expected<Error, PagesAffected>
NoopDmaSpace::NoopVirtualOperations::unmapPages(VirtualAddr, size_t, bool) {
	return PagesAffected{};
}

frg::expected<Error, PagesAffected>
NoopDmaSpace::NoopVirtualOperations::agePages(VirtualAddr, size_t, bool, bool) {
	return PagesAffected{};
}

} // namespace thor
