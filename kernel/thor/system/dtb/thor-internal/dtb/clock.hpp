#pragma once

#include <dtb.hpp>
#include <stdint.h>
#include <thor-internal/irq.hpp>

namespace thor::dt {

struct Clock {
	virtual void enable() = 0;
	virtual void disable() = 0;
	virtual bool setFrequency(uint64_t frequency, uint64_t parentFrequency) = 0;

	Clock *parent{};
	frg::vector<Clock *, KernelAlloc> children{*kernelAlloc};

	uint32_t flags{};
};

} // namespace thor::dt
