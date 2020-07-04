#pragma once

#include <thor-internal/ring-buffer.hpp>

namespace thor {

extern bool wantKernelProfile;

void initializeProfile();
LogRingBuffer *getGlobalProfileRing();

} // namespace thor
