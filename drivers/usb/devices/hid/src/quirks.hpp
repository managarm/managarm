#pragma once

#include <stdint.h>

#include "hid.hpp"

namespace quirks {

void processField(HidDevice *dev, uint16_t usagePage, uint16_t usageId, Field &f);

} // namespace quirks
