#include <frg/allocation.hpp>
#include <frg/hash_map.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/main.hpp>

namespace thor {

namespace {
	frg::eternal<
		frg::hash_map<
			frg::string_view,
			smarter::shared_ptr<KernelIoChannel>,
			frg::hash<frg::string_view>,
			Allocator
		>
	> globalChannelMap{frg::hash<frg::string_view>{}};
}

initgraph::Stage *getIoChannelsDiscoveredStage() {
	static initgraph::Stage s{&globalInitEngine, "general.iochannels-discovered"};
	return &s;
}

void publishIoChannel(smarter::shared_ptr<KernelIoChannel> channel) {
	globalChannelMap->insert(channel->tag(), std::move(channel));
}

smarter::shared_ptr<KernelIoChannel> solicitIoChannel(frg::string_view tag) {
	auto maybeChannel = globalChannelMap->get(tag);
	if(!maybeChannel)
		return nullptr;
	return *maybeChannel;
}

coroutine<void> dumpRingToChannel(LogRingBuffer *ringBuffer,
		smarter::shared_ptr<KernelIoChannel> channel, size_t maxRecordSize) {
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
				auto ioOutcome = co_await channel->issueIo(KernelIoChannel::ioProgressOutput);
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
