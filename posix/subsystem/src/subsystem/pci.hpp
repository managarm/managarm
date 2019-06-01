#pragma once

#include <cofiber.hpp>
#include "../drvcore.hpp"

namespace pci_subsystem {

cofiber::no_future run();

std::shared_ptr<drvcore::Device> getDeviceByMbus(int id);

} // namespace pci_subsystem
