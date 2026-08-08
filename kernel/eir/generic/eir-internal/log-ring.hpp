#pragma once

#include <frg/optional.hpp>
#include <stddef.h>
#include <stdint.h>

namespace eir {

// Upper bound on the size of a log record; matches the stack_buffer_loggers in debug.hpp.
constexpr size_t maxLogLine = 128;

// Single-producer record ring; the wire format matches Thor's LogRingBuffer.
// Eir is single-threaded during its run, so the atomics that Thor needs are not required.
class EirLogRing {
public:
	struct Record {
		uint64_t nextPtr;
		size_t size;
	};

	constexpr EirLogRing(char *buffer, size_t size) : buffer_{buffer}, ringSize_{size} {}

	void enqueue(const void *data, size_t recordSize);

	// Copies at most `maxSize` bytes of the record at `deqPtr`; null once `deqPtr` hits the head.
	frg::optional<Record> dequeueAt(uint64_t deqPtr, void *data, size_t maxSize) const;

	uint64_t headPtr() const { return headPtr_; }
	uint64_t tailPtr() const { return tailPtr_; }

private:
	static constexpr size_t headerSize = sizeof(size_t);
	static constexpr size_t recordAlign = sizeof(size_t);

	static constexpr size_t effectiveSize(size_t recordSize) {
		return (headerSize + recordSize + recordAlign - 1) & ~(recordAlign - 1);
	}

	char *buffer_;
	size_t ringSize_;
	uint64_t tailPtr_ = 0;
	uint64_t headPtr_ = 0;
};

EirLogRing &bootLogRing();

} // namespace eir
