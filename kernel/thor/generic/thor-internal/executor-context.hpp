#pragma once

namespace thor {

struct WorkQueue;

// This class uniquely identifies a thread or fiber.
struct ExecutorContext {
	ExecutorContext();

	ExecutorContext(const ExecutorContext &) = delete;

	ExecutorContext &operator= (const ExecutorContext &) = delete;

	// Used by AdaptiveMutex to test if the context is still active (i.e., running on any CPU).
	std::atomic<bool> active{false};
	WorkQueue *exceptionalWq{nullptr};
};

} // namespace thor
