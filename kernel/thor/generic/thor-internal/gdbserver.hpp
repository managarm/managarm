#pragma once

#include <frg/string.hpp>
#include <thor-internal/thread.hpp>
#include <thor-internal/work-queue.hpp>

namespace thor {

void launchGdbServer(smarter::shared_ptr<Thread, ActiveHandle> thread,
		frg::string_view path, smarter::shared_ptr<WorkQueue> wq);

} // namespace thor
