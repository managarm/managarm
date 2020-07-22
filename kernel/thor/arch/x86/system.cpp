#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/legacy-pc/system.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/arch/rtc.hpp>
#include <thor-internal/arch/system.hpp>

namespace thor {

void initializeExtendedSystem() {
	acpi::initializeExtendedSystem();
	legacy_pc::initializeDevices();
	initializeRtc();
}

} // namespace thor
