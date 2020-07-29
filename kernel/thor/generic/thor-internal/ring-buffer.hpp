#pragma once

#include <stdint.h>
#include <stddef.h>

#include <frg/tuple.hpp>
#include <frg/utility.hpp>
#include <thor-internal/kernel-locks.hpp>

namespace thor {

struct LogRingBuffer {
	LogRingBuffer(uintptr_t storage, size_t size)
	: size_{size}, enqueue_{0}, stor_{reinterpret_cast<char *>(storage)},
	mutex_{} {
		assert(size_ && (size_ & (size_ - 1)) == 0);
	}

	void enqueue(char c) {
		auto irqLock = frigg::guard(&thor::irqMutex());
		auto lock = frigg::guard(&mutex_);
		stor_[enqueue_ & (size_ - 1)] = c;
		enqueue_++;
	}

	frg::tuple<uint64_t, size_t>
	dequeueInto(void *buffer, size_t dequeue, size_t size) {
		auto irqLock = frigg::guard(&thor::irqMutex());
		auto lock = frigg::guard(&mutex_);
		size_t actualSize = frg::min(size, newDataSize(dequeue));
		size_t i = 0;
		while (i < actualSize) {
			size_t readSize = frg::min(
				size_ - ((dequeue + i) & (size_ - 1)),
				actualSize - i
			);

			memcpy(
				reinterpret_cast<char *>(buffer) + i,
				stor_ + ((dequeue + i) & (size_ - 1)),
				readSize
			);

			i += readSize;
		}

		return frg::make_tuple(dequeue + i, actualSize);
	}

	size_t enqueueIndex() {
		auto irqLock = frigg::guard(&thor::irqMutex());
		auto lock = frigg::guard(&mutex_);

		return enqueue_;
	}

	bool hasEnoughBytes(uint64_t dequeue, size_t wantedSize) {
		auto irqLock = frigg::guard(&thor::irqMutex());
		auto lock = frigg::guard(&mutex_);

		return newDataSize(dequeue) >= wantedSize;
	}

	size_t wantedSize(uint64_t dequeue, size_t size) {
		auto irqLock = frigg::guard(&thor::irqMutex());
		auto lock = frigg::guard(&mutex_);

		return frg::min(newDataSize(dequeue), size);
	}

private:
	size_t size_;
	size_t enqueue_;
	char *stor_;

	frigg::TicketLock mutex_;

	size_t newDataSize(size_t dequeue) const {
		return frg::min(enqueue_ - dequeue, size_);
	}
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

	frg::tuple<bool, size_t, uint64_t>
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
			return {false, deqPtr, 0};
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
		return {true, newPtr, chunkSize};
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
