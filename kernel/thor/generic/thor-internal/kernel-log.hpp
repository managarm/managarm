#pragma once

#include <thor-internal/ring-buffer.hpp>

namespace thor {

void initializeLog();

LogRingBuffer *getGlobalLogRing();

} // namespace thor
