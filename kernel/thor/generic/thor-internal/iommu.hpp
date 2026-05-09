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

// Represents a physical page space used by an IOMMU.
// Owns the page tables.
struct DmaPageSpace : PageSpace {
	DmaPageSpace(PhysicalAddr root)
	: PageSpace{root} { }
};

// Provides virtual memory operations (mapping, unmapping, etc.) for a DMA space.
struct DmaOperations : VirtualOperations {
	DmaOperations(DmaPageSpace *pageSpace)
	: pageSpace_{pageSpace} { }

protected:
	DmaPageSpace *pageSpace_;
};

// A virtualized page space used specifically for DMA.
struct DmaSpace : VirtualSpace {
	DmaSpace(VirtualOperations *ops, DmaPageSpace *pageSpace)
	: VirtualSpace{ops}, pageSpace_{pageSpace} { }

	static smarter::shared_ptr<DmaSpace> create(VirtualOperations *ops, DmaPageSpace *pageSpace) {
		auto ptr = smarter::allocate_shared<DmaSpace>(*kernelAlloc, ops, pageSpace);
		ptr->selfPtr = ptr;
		ptr->setupInitialHole(0, 1UL << 39);
		return ptr;
	}

	DmaPageSpace *pageSpace_;
};

}
