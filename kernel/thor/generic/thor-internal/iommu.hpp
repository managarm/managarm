#pragma once

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

	virtual void enableDevice(pci::PciEntity *dev) = 0;

private:
	const size_t id_;
};

}
