#pragma once

namespace thor {

struct WorkQueue;

// This class uniquely identifies a thread or fiber.
struct ExecutorContext {
	ExecutorContext();

	ExecutorContext(const ExecutorContext &) = delete;

	ExecutorContext &operator= (const ExecutorContext &) = delete;

	WorkQueue *exceptionalWq{nullptr};
};

} // namespace thor
