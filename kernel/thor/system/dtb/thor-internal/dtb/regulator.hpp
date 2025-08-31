#pragma once

#include <dtb.hpp>
#include <stdint.h>
#include <thor-internal/irq.hpp>

namespace thor::dt {

struct Regulator {
	virtual void enable() = 0;
	virtual void disable() = 0;
	virtual bool setVoltage(uint64_t microVolts) = 0;
};

} // namespace thor::dt
