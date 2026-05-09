#pragma once

#include <frg/vector.hpp>
#include <thor-internal/address-space.hpp>
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

	virtual void enableDevice(pci::PciEntity *dev, bool passthrough = true) = 0;

private:
	const size_t id_;
};

// A page space used specifically for DMA.
struct DmaSpace : VirtualSpace {
	DmaSpace(VirtualOperations *ops)
	: VirtualSpace{ops} { }
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
