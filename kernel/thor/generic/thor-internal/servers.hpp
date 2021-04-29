#pragma once

#include <thor-internal/coroutine.hpp>
#include <thor-internal/universe.hpp>

namespace thor {

void initializeSvrctl();
void initializeMbusStream();
coroutine<void> runMbus();
coroutine<LaneHandle> runServer(frg::string_view name);

} // namespace thor
