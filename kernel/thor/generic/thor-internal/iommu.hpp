#pragma once

#include <thor-internal/pci/pci.hpp>
#include <thor-internal/debug.hpp>
#include <stddef.h>

struct DeviceTreeProperty;

namespace thor {

namespace pci {

struct PciEntity;

}

struct DeviceTreeNode;

struct Iommu {
	Iommu(size_t id)
	: id_{id} {}

	size_t id() const {
		return id_;
	}

	virtual void enableDevice(pci::PciEntity *dev) = 0;

	virtual void enableDevice(DeviceTreeNode *dev, const DeviceTreeProperty &iommuProp) {
		(void)dev;
		(void)iommuProp;
		panicLogger() << "thor: Iommu::enableDevice(DeviceTreeNode) is not implemented"
				<< frg::endlog;
	}

private:
	const size_t id_;
};

}
