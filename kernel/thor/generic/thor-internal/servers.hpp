#pragma once

#include <thor-internal/coroutine.hpp>
#include <thor-internal/universe.hpp>

namespace thor {

void initializeSvrctl();
void initializeMbusStream();
coroutine<void> initPosixEmulation();
coroutine<void> runMbus();
coroutine<smarter::shared_ptr<Stream, LanePolicy>> runServer(frg::string_view name);

} // namespace thor
