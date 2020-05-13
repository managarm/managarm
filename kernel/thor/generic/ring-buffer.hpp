#pragma once

#include <stdint.h>
#include <stddef.h>
#include <frg/tuple.hpp>
#include <frg/utility.hpp>
#include "../arch/x86/ints.hpp"

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

} // namespace thor
