#pragma once

#include <stdint.h>
#include <stddef.h>

#include <async/recurring-event.hpp>
#include <frg/tuple.hpp>
#include <frg/utility.hpp>
#include <thor-internal/kernel-locks.hpp>

namespace thor {

struct LogRingBuffer {
	LogRingBuffer(uintptr_t storage, size_t size)
	: ringSize_{size}, buffer_{reinterpret_cast<char *>(storage)} {
		assert(ringSize_ && (ringSize_ & (ringSize_ - 1)) == 0);
	}

	auto wait(uint64_t deqPtr) {
		return event_.async_wait_if([=] () -> bool {
			return headPtr_.load(std::memory_order_relaxed) == deqPtr;
		});
	}

	void enqueue(const void *data, size_t recordSize) {
		{
			auto irqLock = frg::guard(&thor::irqMutex());
			auto lock = frg::guard(&mutex_);

			auto p = reinterpret_cast<const char *>(data);
			assert(effectiveSize(recordSize) <= ringSize_);

			auto enqPtr = headPtr_.load(std::memory_order_relaxed);

			// Compute the invalidated part of the ring buffer.
			auto invalPtr = tailPtr_.load(std::memory_order_relaxed);
			while(invalPtr + ringSize_ < enqPtr + headerSize + recordSize) {
				assert(invalPtr < enqPtr);
				auto tailOffset = invalPtr & (ringSize_ - 1);
				// Alignment guarantees that the header fits contiguously.
				assert(!(tailOffset > ringSize_ - headerSize));

				size_t tailSize;
				memcpy(&tailSize, buffer_ + tailOffset, sizeof(size_t));
				assert(tailSize <= ringSize_);

				invalPtr += effectiveSize(tailSize);
			}

			// Invalidate the ring *before* writing to it.
			assert(!(invalPtr & (recordAlign - 1)));
			tailPtr_.store(invalPtr, std::memory_order_release);

			// Copy to the ring.
			auto recordOffset = enqPtr & (ringSize_ - 1);
			// Alignment guarantees that the header fits contiguously.
			assert(!(recordOffset > ringSize_ - headerSize));

			memcpy(buffer_ + recordOffset, &recordSize, sizeof(size_t));
			auto preWrapSize = frg::min(ringSize_ - (recordOffset + headerSize), recordSize);
			memcpy(buffer_ + recordOffset + sizeof(size_t), p, preWrapSize);
			memcpy(buffer_, p + preWrapSize, recordSize - preWrapSize);

			// Commit the operation *after* writing to the ring.
			auto commitPtr = enqPtr + effectiveSize(recordSize);
			headPtr_.store(commitPtr, std::memory_order_release);
		}

		event_.raise();
	}

	void enqueue(char c) {
		enqueue(&c, 1);
	}

	frg::tuple<bool, uint64_t, uint64_t, size_t>
	dequeueAt(uint64_t deqPtr, void *data, size_t maxSize) {
		auto p = reinterpret_cast<char *>(data);

	tryAgain:
		// Find a valid position to dequeue from.
		auto beforePtr = tailPtr_.load(std::memory_order_relaxed);
		if(deqPtr < beforePtr)
			deqPtr = beforePtr;

		auto validPtr = headPtr_.load(std::memory_order_acquire);
		if(deqPtr == validPtr)
			return {false, deqPtr, deqPtr, 0};
		assert(deqPtr < validPtr);

		// Copy from the ring.
		auto recordOffset = deqPtr & (ringSize_ - 1);
		// Alignment guarantees that the header fits contiguously.
		assert(!(recordOffset > ringSize_ - headerSize));

		size_t recordSize;
		memcpy(&recordSize, buffer_ + recordOffset, sizeof(size_t));
		// Alignment guarantees that the header fits contiguously.
		if(recordSize > ringSize_ - headerSize)
			goto tryAgain;
		auto chunkSize = frg::min(recordSize, maxSize);
		auto preWrapSize = frg::min(ringSize_ - (recordOffset + headerSize), chunkSize);
		memcpy(p, buffer_ + recordOffset + sizeof(size_t), preWrapSize);
		memcpy(p + preWrapSize, buffer_, chunkSize - preWrapSize);

		// Validate the data *after* copying.
		auto afterPtr = tailPtr_.load(std::memory_order_acquire);
		if(deqPtr < afterPtr)
			goto tryAgain;

		size_t newPtr = deqPtr + effectiveSize(recordSize);
		return {true, deqPtr, newPtr, chunkSize};
	}

private:
	static constexpr size_t headerSize = sizeof(size_t);
	static constexpr size_t recordAlign = sizeof(size_t);

	size_t effectiveSize(size_t recordSize) {
		return (headerSize + recordSize + recordAlign - 1) & ~(recordAlign - 1);
	}

	frg::ticket_spinlock mutex_;

	// This allows consumers to wait until new records arrive.
	async::recurring_event event_;

	size_t ringSize_;
	char *buffer_;
	std::atomic<uint64_t> tailPtr_{0};
	std::atomic<uint64_t> headPtr_{0};
};

struct SingleContextRecordRing {
	void enqueue(const void *data, size_t recordSize) {
		auto ringSize = size_t{1} << shift_;
		auto p = reinterpret_cast<const char *>(data);
		assert(effectiveSize(recordSize) <= ringSize);

		auto enqPtr = headPtr_.load(std::memory_order_relaxed);

		// Compute the invalidated part of the ring buffer.
		auto invalPtr = tailPtr_.load(std::memory_order_relaxed);
		while(invalPtr + ringSize < enqPtr + headerSize + recordSize) {
			assert(invalPtr < enqPtr);
			auto tailOffset = invalPtr & (ringSize - 1);
			// Alignment guarantees that the header fits contiguously.
			assert(!(tailOffset > ringSize - headerSize));

			size_t tailSize;
			memcpy(&tailSize, buffer_ + tailOffset, sizeof(size_t));
			assert(tailSize <= ringSize);

			invalPtr += effectiveSize(tailSize);
		}

		// Invalidate the ring *before* writing to it.
		assert(!(invalPtr & (recordAlign - 1)));
		tailPtr_.store(invalPtr, std::memory_order_release);

		// Copy to the ring.
		auto recordOffset = enqPtr & (ringSize - 1);
		// Alignment guarantees that the header fits contiguously.
		assert(!(recordOffset > ringSize - headerSize));

		memcpy(buffer_ + recordOffset, &recordSize, sizeof(size_t));
		auto preWrapSize = frg::min(ringSize - (recordOffset + headerSize), recordSize);
		memcpy(buffer_ + recordOffset + sizeof(size_t), p, preWrapSize);
		memcpy(buffer_, p + preWrapSize, recordSize - preWrapSize);

		// Commit the operation *after* writing to the ring.
		auto commitPtr = enqPtr + effectiveSize(recordSize);
		headPtr_.store(commitPtr, std::memory_order_release);
	}

	frg::tuple<bool, uint64_t, uint64_t, size_t>
	dequeueAt(uint64_t deqPtr, void *data, size_t maxSize) {
		auto ringSize = size_t{1} << shift_;
		auto p = reinterpret_cast<char *>(data);

	tryAgain:
		// Find a valid position to dequeue from.
		auto beforePtr = tailPtr_.load(std::memory_order_relaxed);
		if(deqPtr < beforePtr)
			deqPtr = beforePtr;

		auto validPtr = headPtr_.load(std::memory_order_acquire);
		if(deqPtr == validPtr)
			return {false, deqPtr, deqPtr, 0};
		assert(deqPtr < validPtr);

		// Copy from the ring.
		auto recordOffset = deqPtr & (ringSize - 1);
		// Alignment guarantees that the header fits contiguously.
		assert(!(recordOffset > ringSize - headerSize));

		size_t recordSize;
		memcpy(&recordSize, buffer_ + recordOffset, sizeof(size_t));
		// Alignment guarantees that the header fits contiguously.
		if(recordSize > ringSize - headerSize)
			goto tryAgain;
		auto chunkSize = frg::min(recordSize, maxSize);
		auto preWrapSize = frg::min(ringSize - (recordOffset + headerSize), chunkSize);
		memcpy(p, buffer_ + recordOffset + sizeof(size_t), preWrapSize);
		memcpy(p + preWrapSize, buffer_, chunkSize - preWrapSize);

		// Validate the data *after* copying.
		auto afterPtr = tailPtr_.load(std::memory_order_acquire);
		if(deqPtr < afterPtr)
			goto tryAgain;

		size_t newPtr = deqPtr + effectiveSize(recordSize);
		return {true, deqPtr, newPtr, chunkSize};
	}

private:
	static constexpr size_t headerSize = sizeof(size_t);
	static constexpr size_t recordAlign = sizeof(size_t);

	size_t effectiveSize(size_t recordSize) {
		return (headerSize + recordSize + recordAlign - 1) & ~(recordAlign - 1);
	}

	int shift_ = 16;
	char buffer_[1 << 16];
	std::atomic<uint64_t> tailPtr_{0};
	std::atomic<uint64_t> headPtr_{0};
};

} // namespace thor
