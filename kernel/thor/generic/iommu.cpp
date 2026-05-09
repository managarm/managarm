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

} // namespace thor
