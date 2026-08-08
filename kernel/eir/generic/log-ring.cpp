#include <assert.h>

#include <eir-internal/log-ring.hpp>
#include <frg/utility.hpp>
#include <string.h>

namespace eir {

namespace {

// 32 KiB is more than enough to back a full screen of text.
constexpr size_t bootLogRingSize = 0x8000;
static_assert(bootLogRingSize && !(bootLogRingSize & (bootLogRingSize - 1)));

alignas(8) constinit char bootLogRingBuffer[bootLogRingSize];

constinit EirLogRing logRing{bootLogRingBuffer, bootLogRingSize};

} // anonymous namespace

void EirLogRing::enqueue(const void *data, size_t recordSize) {
	auto p = reinterpret_cast<const char *>(data);
	assert(effectiveSize(recordSize) <= ringSize_);

	auto enqPtr = headPtr_;

	// Compute the invalidated part of the ring buffer.
	auto invalPtr = tailPtr_;
	while (invalPtr + ringSize_ < enqPtr + headerSize + recordSize) {
		assert(invalPtr < enqPtr);
		auto tailOffset = static_cast<size_t>(invalPtr & (ringSize_ - 1));
		// Alignment guarantees that the header fits contiguously.
		assert(!(tailOffset > ringSize_ - headerSize));

		size_t tailSize;
		memcpy(&tailSize, buffer_ + tailOffset, sizeof(size_t));
		assert(tailSize <= ringSize_);

		invalPtr += effectiveSize(tailSize);
	}

	// Invalidate the ring *before* writing to it.
	assert(!(invalPtr & (recordAlign - 1)));
	tailPtr_ = invalPtr;

	// Copy to the ring.
	auto recordOffset = static_cast<size_t>(enqPtr & (ringSize_ - 1));
	// Alignment guarantees that the header fits contiguously.
	assert(!(recordOffset > ringSize_ - headerSize));

	memcpy(buffer_ + recordOffset, &recordSize, sizeof(size_t));
	auto preWrapSize = frg::min(ringSize_ - (recordOffset + headerSize), recordSize);
	memcpy(buffer_ + recordOffset + sizeof(size_t), p, preWrapSize);
	memcpy(buffer_, p + preWrapSize, recordSize - preWrapSize);

	// Commit the operation *after* writing to the ring.
	headPtr_ = enqPtr + effectiveSize(recordSize);
}

frg::optional<EirLogRing::Record>
EirLogRing::dequeueAt(uint64_t deqPtr, void *data, size_t maxSize) const {
	auto p = reinterpret_cast<char *>(data);

	// Records before the tail have been invalidated by now.
	if (deqPtr < tailPtr_)
		deqPtr = tailPtr_;
	if (deqPtr == headPtr_)
		return frg::null_opt;
	assert(deqPtr < headPtr_);

	// Copy from the ring.
	auto recordOffset = static_cast<size_t>(deqPtr & (ringSize_ - 1));
	// Alignment guarantees that the header fits contiguously.
	assert(!(recordOffset > ringSize_ - headerSize));

	size_t recordSize;
	memcpy(&recordSize, buffer_ + recordOffset, sizeof(size_t));
	assert(recordSize <= ringSize_ - headerSize);

	auto chunkSize = frg::min(recordSize, maxSize);
	auto preWrapSize = frg::min(ringSize_ - (recordOffset + headerSize), chunkSize);
	memcpy(p, buffer_ + recordOffset + sizeof(size_t), preWrapSize);
	memcpy(p + preWrapSize, buffer_, chunkSize - preWrapSize);

	return Record{deqPtr + effectiveSize(recordSize), chunkSize};
}

EirLogRing &bootLogRing() { return logRing; }

} // namespace eir
