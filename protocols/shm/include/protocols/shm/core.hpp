#pragma once

// Memory layout and wait-free core of the ring buffers of the shm protocol (see shm.bragi).
// This header is freestanding such that it can be shared between the kernel and userspace.
// None of the classes here perform any synchronization among multiple users of the same core.

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>

namespace protocols::shm {

// Values match managarm::shm::Mode.
enum class Mode : int32_t {
	reliable = 1,
};

// Values match managarm::shm::Framing.
enum class Framing : int32_t {
	stream = 1,
};

// Offset of the data area within the producer memory.
inline constexpr size_t dataOffset = 0x1000;
inline constexpr size_t consumerMemorySize = 0x1000;

// Header of the producer memory. Only written by the producer.
struct ProducerHeader {
	// Everything before this position is committed.
	std::atomic<uint64_t> head;
	// Reserved for modes in which the producer invalidates data.
	std::atomic<uint64_t> reserved;
	// The consumer should not wait for its watermark as long as it is behind this position.
	std::atomic<uint64_t> flushed;
	// The producer wants a kick once consumed reaches this position.
	std::atomic<uint64_t> wakeAt;
};

// Header of the consumer memory. Only written by the consumer.
struct ConsumerHeader {
	// Everything before this position can be overwritten.
	std::atomic<uint64_t> consumed;
	// The consumer wants a kick once head reaches this position.
	std::atomic<uint64_t> wakeAt;
};

static_assert(sizeof(ProducerHeader) <= dataOffset);
static_assert(sizeof(ConsumerHeader) <= consumerMemorySize);

inline constexpr bool isValidRingSize(uint64_t size) {
	return size >= dataOffset && !(size & (size - 1));
}

// True if moving a position from oldPosition to newPosition requires a kick.
inline constexpr bool passesWakeAt(uint64_t wakeAt, uint64_t oldPosition, uint64_t newPosition) {
	return oldPosition < wakeAt && wakeAt <= newPosition;
}

struct WritableSpan {
	std::byte *data;
	size_t size;
};

struct ReadableSpan {
	const std::byte *data;
	size_t size;
};

struct ProducerCore {
	ProducerCore() = default;

	ProducerCore(ProducerHeader *header, const ConsumerHeader *consumerHeader,
			std::byte *data, size_t ringSize)
	: header_{header}, consumerHeader_{consumerHeader},
			data_{data}, ringSize_{ringSize} {
		assert(isValidRingSize(ringSize));
	}

	size_t ringSize() const { return ringSize_; }

	uint64_t head() const { return head_; }

	// Picks up the progress of the consumer.
	// Returns false if the consumer violates the protocol.
	// seq_cst since armSpace() relies on the ordering against the store to wakeAt.
	[[nodiscard]] bool refresh() {
		auto consumed = consumerHeader_->consumed.load(std::memory_order_seq_cst);
		if(consumed < consumed_ || consumed > head_)
			return false;
		consumed_ = consumed;
		return true;
	}

	// Free space as of the last refresh().
	size_t freeSize() const {
		return ringSize_ - (head_ - consumed_);
	}

	// Contiguous part of the free space.
	WritableSpan writableSpan() const {
		auto offset = head_ & (ringSize_ - 1);
		auto size = freeSize();
		if(size > ringSize_ - offset)
			size = ringSize_ - offset;
		return {data_ + offset, size};
	}

	// Commits bytes that were written to writableSpan().
	void produce(size_t size) {
		assert(size <= freeSize());
		commit_(head_ + size);
	}

	// Asks the consumer to process everything that was committed so far, regardless of its
	// watermark. Returns true if the caller needs to check needsKickAfterFlush().
	[[nodiscard]] bool flush() {
		if(flushed_ == head_)
			return false;
		flushed_ = head_;
		header_->flushed.store(flushed_, std::memory_order_seq_cst);
		return true;
	}

	// To be called after the head moved on from oldHead.
	bool needsKick(uint64_t oldHead) const {
		return passesWakeAt(consumerHeader_->wakeAt.load(std::memory_order_seq_cst),
				oldHead, head_);
	}

	// To be called after flush() returned true.
	bool needsKickAfterFlush() const {
		return consumerHeader_->wakeAt.load(std::memory_order_seq_cst) > head_;
	}

	// Prepares to wait until the given amount of space is free.
	// Returns false if there is no need to wait; performs a refresh().
	[[nodiscard]] bool armSpace(size_t size, bool &violation) {
		assert(size <= ringSize_);
		violation = !refresh();
		if(violation || freeSize() >= size)
			return false;
		header_->wakeAt.store(head_ + size - ringSize_, std::memory_order_seq_cst);
		violation = !refresh();
		return !violation && freeSize() < size;
	}

private:
	void commit_(uint64_t newHead) {
		head_ = newHead;
		// Commit the operation *after* writing to the ring.
		header_->head.store(newHead, std::memory_order_seq_cst);
	}

	ProducerHeader *header_ = nullptr;
	const ConsumerHeader *consumerHeader_ = nullptr;
	std::byte *data_ = nullptr;
	size_t ringSize_ = 0;

	// The shared copies of our own positions are never read back.
	uint64_t head_ = 0;
	uint64_t flushed_ = 0;
	// Validated copy of the consumer's position.
	uint64_t consumed_ = 0;
};

struct ConsumerCore {
	ConsumerCore() = default;

	ConsumerCore(const ProducerHeader *producerHeader, ConsumerHeader *header,
			const std::byte *data, size_t ringSize)
	: producerHeader_{producerHeader}, header_{header},
			data_{data}, ringSize_{ringSize} {
		assert(isValidRingSize(ringSize));
	}

	size_t ringSize() const { return ringSize_; }

	// Position of the next byte that is consumed.
	uint64_t position() const { return position_; }

	// Picks up the progress of the producer.
	// Returns false if the producer violates the protocol.
	[[nodiscard]] bool refresh() {
		auto head = producerHeader_->head.load(std::memory_order_seq_cst);
		if(head < head_ || head > position_ + ringSize_)
			return false;
		head_ = head;
		return true;
	}

	// Number of available bytes as of the last refresh().
	size_t availableSize() const {
		return head_ - position_;
	}

	// Contiguous part of the available bytes.
	ReadableSpan readableSpan() const {
		auto offset = position_ & (ringSize_ - 1);
		auto size = availableSize();
		if(size > ringSize_ - offset)
			size = ringSize_ - offset;
		return {data_ + offset, size};
	}

	// Frees bytes that were taken from readableSpan().
	// Returns true if the producer needs a kick.
	[[nodiscard]] bool consume(size_t size) {
		assert(size <= availableSize());
		auto oldPosition = position_;
		position_ += size;
		header_->consumed.store(position_, std::memory_order_seq_cst);
		return passesWakeAt(producerHeader_->wakeAt.load(std::memory_order_seq_cst),
				oldPosition, position_);
	}

	// Prepares to wait until watermark bytes are available or until the producer flushes.
	// Returns false if there is no need to wait.
	[[nodiscard]] bool armData(size_t watermark, bool &violation) {
		assert(watermark && watermark <= ringSize_);
		header_->wakeAt.store(position_ + watermark, std::memory_order_seq_cst);

		violation = !refresh();
		if(violation || head_ >= position_ + watermark)
			return false;
		auto flushed = producerHeader_->flushed.load(std::memory_order_seq_cst);
		return flushed <= position_;
	}

private:
	const ProducerHeader *producerHeader_ = nullptr;
	ConsumerHeader *header_ = nullptr;
	const std::byte *data_ = nullptr;
	size_t ringSize_ = 0;

	// The shared copy of our own position is never read back.
	uint64_t position_ = 0;
	// Validated copy of the producer's position.
	uint64_t head_ = 0;
};

} // namespace protocols::shm
