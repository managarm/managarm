#pragma once

#include <thor-internal/coroutine.hpp>
#include <thor-internal/debug.hpp>
#include <thor-internal/ring-buffer.hpp>

namespace thor {

void postLogRecord(frg::string_view record, bool expedited);
coroutine<void> waitForLog(uint64_t deqPtr);
frg::tuple<bool, uint64_t, uint64_t, size_t> retrieveLogRecord(uint64_t deqPtr, void *data, size_t maxSize);

LogRingBuffer *getGlobalKmsgRing();

} // namespace thor
