#pragma once

#include <thor-internal/coroutine.hpp>

namespace thor {

void initializeSvrctl();
coroutine<void> runMbus();
coroutine<LaneHandle> runServer(frg::string_view name);

} // namespace thor
