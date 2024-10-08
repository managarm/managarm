#pragma once

#include <async/result.hpp>
#include "../drvcore.hpp"

namespace acpi_subsystem {

async::detached run();

std::shared_ptr<drvcore::Device> getDeviceByMbus(int id);

} // namespace acpi_subsystem
