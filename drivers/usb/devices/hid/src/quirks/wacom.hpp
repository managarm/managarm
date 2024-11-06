#pragma once

#include "../hid.hpp"

namespace quirks::wacom {

void touchHidLimits(uint16_t usagePage, uint16_t usageId, Field &f);

} // namespace quirks::wacom
