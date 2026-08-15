#pragma once

#include <dtb.hpp>
#include <stdint.h>
#include <thor-internal/irq.hpp>

namespace thor::dt {

struct IrqController {
	// Resolve a DT interrupt specifier to an IRQ.
	virtual smarter::shared_ptr<IrqPin> resolveDtIrq(dtb::Cells irq) = 0;

	// Resolve a controller-specific IRQ index to an IRQ.
	virtual smarter::shared_ptr<IrqPin> resolveIrqIndex(uint64_t index) = 0;
};

} // namespace thor::dt
