#include <async/recurring-event.hpp>
#include <frg/allocation.hpp>
#include <frg/hash_map.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/main.hpp>

namespace thor {

namespace {
	constinit IrqSpinlock globalChannelMutex;

	// Protected by globalChannelMutex.
	frg::eternal<
		frg::hash_map<
			frg::string_view,
			smarter::shared_ptr<KernelIoChannel>,
			frg::hash<frg::string_view>,
			Allocator
		>
	> globalChannelMap{frg::hash<frg::string_view>{}};

	// Raised whenever a channel is published.
	constinit async::recurring_event globalChannelEvent;

	smarter::shared_ptr<KernelIoChannel> lookupChannel(frg::string_view tag) {
		auto lock = frg::guard(&globalChannelMutex);
		auto maybeChannel = globalChannelMap->get(tag);
		if(!maybeChannel)
			return nullptr;
		return *maybeChannel;
	}
}

initgraph::Stage *getIoChannelsDiscoveredStage() {
	static initgraph::Stage s{&globalInitEngine, "general.iochannels-discovered"};
	return &s;
}

void publishIoChannel(smarter::shared_ptr<KernelIoChannel> channel) {
	{
		auto lock = frg::guard(&globalChannelMutex);
		if(globalChannelMap->get(channel->tag())) {
			warningLogger() << "thor: Ignoring duplicate I/O channel "
					<< channel->descriptiveTag() << frg::endlog;
			return;
		}
		globalChannelMap->insert(channel->tag(), std::move(channel));
	}
	// Waiters resume inline, so raise outside of the lock.
	globalChannelEvent.raise();
}

coroutine<smarter::shared_ptr<KernelIoChannel>> solicitIoChannel(frg::string_view tag) {
	while(true) {
		if(auto channel = lookupChannel(tag))
			co_return channel;
		co_await globalChannelEvent.async_wait_if([&] () -> bool {
			return !lookupChannel(tag);
		});
	}
}

coroutine<void> dumpRingToChannel(LogRingBuffer *ringBuffer,
		frg::string_view tag, size_t maxRecordSize) {
	auto channel = co_await solicitIoChannel(tag);
	infoLogger() << "thor: Connecting " << tag << " to I/O channel "
			<< channel->descriptiveTag() << frg::endlog;

	// One extra byte distinguishes records of exactly maxRecordSize from truncated ones.
	frg::unique_memory<KernelAlloc> record{*kernelAlloc, maxRecordSize + 1};
	uint64_t currentPtr = 0;
	bool unflushed = false;
	while(true) {
		auto [success, recordPtr, nextPtr, actualSize] = ringBuffer->dequeueAt(
				currentPtr, record.data(), maxRecordSize + 1);
		if(!success) {
			// Do not leave output in the channel while we block on the ring.
			if(unflushed) {
				auto ioOutcome = co_await channel->issueIo(
						KernelIoChannel::ioProgressOutput | KernelIoChannel::ioFlush);
				assert(ioOutcome);
				unflushed = false;
			}
			co_await ringBuffer->wait(nextPtr);
			continue;
		}
		assert(actualSize); // For now, we do not support size zero records.
		if(recordPtr != currentPtr)
			infoLogger() << "thor: Up to " << (recordPtr - currentPtr)
					<< " lost on I/O channel "
					<< channel->descriptiveTag() << frg::endlog;
		if(actualSize > maxRecordSize) {
			infoLogger() << "thor: Packet truncated on I/O channel "
					<< channel->descriptiveTag() << frg::endlog;
			actualSize = maxRecordSize;
		}
		currentPtr = nextPtr;

		// Records can be larger than the channel's span, so copy them in chunks.
		size_t progress = 0;
		while(progress < actualSize) {
			auto span = channel->writableSpan();
			if(!span.size()) {
				auto ioOutcome = co_await channel->issueIo(KernelIoChannel::ioProgressOutput);
				assert(ioOutcome);
				unflushed = false;
				continue;
			}
			auto chunk = frg::min(span.size(), actualSize - progress);
			memcpy(span.data(), static_cast<std::byte *>(record.data()) + progress, chunk);
			channel->produceOutput(chunk);
			unflushed = true;
			progress += chunk;
		}
	}
}

} // namespace thor
