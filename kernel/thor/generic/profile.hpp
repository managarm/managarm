#pragma once

#include "ring-buffer.hpp"

namespace thor {

extern bool wantKernelProfile;

void initializeProfile();
LogRingBuffer *getGlobalProfileRing();

} // namespace thor
