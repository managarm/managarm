#pragma once

namespace thor {

// This class uniquely identifies a thread or fiber.
struct ExecutorContext {
	ExecutorContext();

	ExecutorContext(const ExecutorContext &) = delete;

	ExecutorContext &operator=(const ExecutorContext &) = delete;
};

inline ExecutorContext *illegalExecutorContext() {
	return reinterpret_cast<ExecutorContext *>(static_cast<uintptr_t>(-1));
}

} // namespace thor
