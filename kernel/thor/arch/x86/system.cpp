#include <thor-internal/acpi/acpi.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/arch/ints.hpp>
#include <thor-internal/arch/pic.hpp>
#include <thor-internal/arch/rtc.hpp>
#include <thor-internal/arch/system.hpp>
#include <thor-internal/legacy-pc/system.hpp>

namespace thor {

void initializeArchitecture() {
	setupBootCpuContext();
	setupEarlyInterruptHandlers();
}

void initializeExtendedSystem() {
	acpi::initializeExtendedSystem();
	legacy_pc::initializeDevices();
	initializeRtc();
}

} // namespace thor
