#pragma once

#include <thor-internal/coroutine.hpp>

namespace thor {

void initializeSvrctl();
void initializeMbusStream();
coroutine<void> runMbus();
coroutine<LaneHandle> runServer(frg::string_view name);

} // namespace thor
