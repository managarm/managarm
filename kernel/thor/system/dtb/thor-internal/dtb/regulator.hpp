#pragma once

#include <dtb.hpp>
#include <stdint.h>
#include <thor-internal/irq.hpp>

namespace thor::dt {

struct Regulator {
	virtual ~Regulator() = default;

	void enable();
	void disable();
	bool isEnabled();
	bool setVoltage(uint64_t newMicroVolts);

protected:
	virtual void hwEnable() = 0;
	virtual void hwDisable() = 0;
	virtual bool hwIsEnabled() = 0;
	virtual bool hwSetVoltage(uint64_t newMicroVolts) = 0;

public:
	enum {
		flagDisableForVoltageChange = 1 << 0
	};

	uint32_t flags{};

private:
	uint32_t enableCount_{};
};

} // namespace thor::dt
