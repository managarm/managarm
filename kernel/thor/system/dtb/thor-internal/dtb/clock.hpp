#pragma once

#include <dtb.hpp>
#include <stdint.h>
#include <thor-internal/irq.hpp>

namespace thor::dt {

struct Clock {
	virtual ~Clock() = default;

	void enable();
	void disable();
	bool isEnabled();
	bool setFrequency(uint64_t newFrequency);
	bool setParent(size_t newParentIndex);

protected:
	virtual void hwEnable() = 0;
	virtual void hwDisable() = 0;
	virtual bool hwIsEnabled() = 0;

	virtual uint32_t hwGetFrequency() = 0;
	virtual bool hwSetFrequency(uint64_t newFrequency) = 0;

	virtual bool hwSetParent(size_t parentIndex) = 0;

public:
	Clock *parent{};
	frg::vector<Clock *, KernelAlloc> parents{*kernelAlloc};

	enum {
		flagDisableForFreqChange = 1 << 0,
		flagDisableForParentChange = 1 << 1
	};

	uint32_t frequency{};
	uint32_t flags{};

private:
	uint32_t enableCount_{};
};

} // namespace thor::dt
