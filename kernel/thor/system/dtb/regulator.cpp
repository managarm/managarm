#include <thor-internal/dtb/regulator.hpp>

namespace thor::dt {

void Regulator::enable() {
	if (enableCount_++ != 0)
		return;

	if (!hwIsEnabled())
		hwEnable();
}

void Regulator::disable() {
	if (--enableCount_ != 0)
		return;

	assert(hwIsEnabled());
	hwDisable();
}

bool Regulator::isEnabled() {
	return enableCount_ || hwIsEnabled();
}

bool Regulator::setVoltage(uint64_t newMicroVolts) {
	bool disable = (flags & flagDisableForVoltageChange) && isEnabled();
	if (disable)
		hwDisable();

	bool res = hwSetVoltage(newMicroVolts);

	if (disable)
		hwEnable();

	return res;
}

} // namespace thor::dt
