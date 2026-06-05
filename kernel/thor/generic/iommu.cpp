#include <thor-internal/iommu.hpp>

namespace {

size_t globalIommuDomainId = 0;

}

namespace thor {

IommuDomain::IommuDomain(smarter::shared_ptr<DmaSpace> space)
: id_{globalIommuDomainId++}, space_{std::move(space)} {}

void IommuDomain::addMember(pci::PciEntity *e) {
	members_.push_back(e);
	e->iommuDomain = this;
}

void NoopDmaSpace::NoopVirtualOperations::retire(RetireNode *) {}

bool NoopDmaSpace::NoopVirtualOperations::submitShootdown(ShootNode *) { return false; }

frg::expected<Error, PagesAffected> NoopDmaSpace::NoopVirtualOperations::mapPresentPages(
    VirtualAddr, MemoryView *, uintptr_t, size_t, PageFlags, CachingMode
) {
	return PagesAffected{};
}

frg::expected<Error, PagesAffected>
NoopDmaSpace::NoopVirtualOperations::restrictPages(VirtualAddr, size_t, PageFlags, CachingMode) {
	return PagesAffected{};
}

frg::expected<Error, PagesAffected> NoopDmaSpace::NoopVirtualOperations::faultPage(
    VirtualAddr, MemoryView *, uintptr_t, FetchFlags, PageFlags, CachingMode
) {
	return PagesAffected{};
}

frg::expected<Error, PagesAffected>
NoopDmaSpace::NoopVirtualOperations::cleanPages(VirtualAddr, size_t) {
	return PagesAffected{};
}

frg::expected<Error, PagesAffected>
NoopDmaSpace::NoopVirtualOperations::unmapPages(VirtualAddr, size_t) {
	return PagesAffected{};
}

frg::expected<Error, PagesAffected>
NoopDmaSpace::NoopVirtualOperations::agePages(VirtualAddr, size_t, bool) {
	return PagesAffected{};
}

} // namespace thor
