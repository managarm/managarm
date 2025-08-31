#include <thor-internal/dtb/clock.hpp>

#include <assert.h>

namespace thor::dt {

void Clock::enable() {
	if (enableCount_++ != 0)
		return;

	if (!hwIsEnabled())
		hwEnable();
}

void Clock::disable() {
	if (--enableCount_ != 0)
		return;

	assert(hwIsEnabled());
	hwDisable();
}

bool Clock::isEnabled() {
	return enableCount_ || hwIsEnabled();
}

bool Clock::setFrequency(uint64_t newFrequency) {
	bool disable = (flags & flagDisableForFreqChange) && isEnabled();
	if (disable)
		hwDisable();

	bool res = hwSetFrequency(newFrequency);

	if (disable)
		hwEnable();

	return res;
}

bool Clock::setParent(size_t newParentIndex) {
	bool disable = (flags & flagDisableForParentChange) && isEnabled();
	if (disable)
		hwDisable();

	bool res = hwSetParent(newParentIndex);

	if (disable)
		hwEnable();

	if (res) {
		parent = parents[newParentIndex];
	}

	return res;
}

} // namespace thor::dt

