#pragma once

#include <expected>

#include <frg/span.hpp>
#include <protocols/shm/core.hpp>
#include <thor-internal/coroutine.hpp>
#include <thor-internal/error.hpp>
#include <thor-internal/event.hpp>
#include <thor-internal/memory-view.hpp>
#include <thor-internal/stream.hpp>

// Kernel side of the shm protocol (see protocols/shm/shm.bragi). The kernel is always the
// provider of the rings.

namespace thor {

// Memory and kick events of a ring before the kernel takes its role; see kernel-shm.cpp.
struct KernelRing;

// A ring that the kernel produces to.
struct KernelProducer {
	// Performs the shm handshake of a ring that the kernel produces to over a conversation
	// lane. The size must be a power of two and a multiple of the page size.
	// Answers with MISMATCH and fails with protocolViolation if userspace requests another ring.
	static coroutine<frg::expected<Error, KernelProducer>> provide(
			smarter::shared_ptr<Stream, LanePolicy> conversation,
			protocols::shm::Mode mode, protocols::shm::Framing framing, size_t size);

	size_t size() const { return core_.ringSize(); }

	// Free space as of the last awaitSpace().
	size_t freeSize() const { return core_.freeSize(); }

	// Contiguous part of the free space.
	frg::span<std::byte> writableSpan() const {
		auto span = core_.writableSpan();
		return {span.data, span.size};
	}

	// Commits bytes that were written to writableSpan().
	void produce(size_t size);

	// Asks userspace to process everything that was committed so far, regardless of its
	// watermark.
	void flush();

	// Waits until the given amount of space is free. Fails with endOfLane if userspace is gone.
	coroutine<frg::expected<Error>> awaitSpace(size_t size);

private:
	KernelProducer(KernelRing ring);

	ImmediateWindow producerWindow_;
	ImmediateWindow consumerWindow_;
	protocols::shm::ProducerCore core_;
	// Raised by us to kick the consumer.
	smarter::shared_ptr<SequencedEvent, TwoPeerPolicy> event_;
	// Raised by the consumer to kick us.
	smarter::shared_ptr<SequencedEvent, TwoPeerPolicy> consumerEvent_;
};

// A ring that the kernel consumes from.
struct KernelConsumer {
	// Performs the shm handshake of a ring that the kernel consumes from over a conversation
	// lane. The size must be a power of two and a multiple of the page size.
	// Answers with MISMATCH and fails with protocolViolation if userspace requests another ring.
	static coroutine<frg::expected<Error, KernelConsumer>> provide(
			smarter::shared_ptr<Stream, LanePolicy> conversation,
			protocols::shm::Mode mode, protocols::shm::Framing framing, size_t size);

	size_t size() const { return core_.ringSize(); }

	// Number of available bytes as of the last awaitData().
	size_t availableSize() const { return core_.availableSize(); }

	// Contiguous part of the available bytes.
	frg::span<const std::byte> readableSpan() const {
		auto span = core_.readableSpan();
		return {span.data, span.size};
	}

	// Frees bytes that were taken from readableSpan().
	void consume(size_t size);

	// Waits until watermark bytes are available or until userspace flushes.
	// Fails with endOfLane if userspace is gone.
	coroutine<frg::expected<Error>> awaitData(size_t watermark);

private:
	KernelConsumer(KernelRing ring);

	ImmediateWindow producerWindow_;
	ImmediateWindow consumerWindow_;
	protocols::shm::ConsumerCore core_;
	// Raised by the producer to kick us.
	smarter::shared_ptr<SequencedEvent, TwoPeerPolicy> producerEvent_;
	// Raised by us to kick the producer.
	smarter::shared_ptr<SequencedEvent, TwoPeerPolicy> event_;
};

} // namespace thor
