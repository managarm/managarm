#include <thor-internal/iommu.hpp>

namespace thor {

void IommuDomain::addMember(pci::PciEntity *e) {
	members_.push_back(e);
	e->iommuDomain = this;
}

} // namespace thor
