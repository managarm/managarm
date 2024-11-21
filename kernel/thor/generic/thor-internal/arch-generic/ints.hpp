#pragma once

#include <thor-internal/arch/ints.hpp>

namespace thor {

struct CpuData;

void sendPingIpi(CpuData *dstData);
void sendShootdownIpi();
void sendSelfCallIpi();

} // namespace thor
