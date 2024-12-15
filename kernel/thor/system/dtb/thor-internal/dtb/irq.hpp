#pragma once

#include <dtb.hpp>
#include <stdint.h>
#include <thor-internal/irq.hpp>

namespace thor::dt {

struct IrqController {
	// Resolve a DT interrupt specifier to an IRQ.
	virtual IrqPin *resolveDtIrq(dtb::Cells irq) = 0;
};

} // namespace thor::dt
