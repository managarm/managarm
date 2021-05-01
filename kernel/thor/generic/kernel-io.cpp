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

// Packets larger than packetSize may be truncated.
coroutine<void> dumpRingToChannel(LogRingBuffer *ringBuffer,
		smarter::shared_ptr<KernelIoChannel> channel, size_t packetSize) {
	uint64_t currentPtr = 0;
	while(true) {
		auto span = channel->writableSpan();
		if(span.size() < packetSize) {
			auto ioOutcome = co_await channel->issueIo(KernelIoChannel::ioProgressOutput);
			assert(ioOutcome);
			continue;
		}

		size_t progress = 0;
		while(progress < span.size()) {
			auto [success, recordPtr, nextPtr, actualSize] = ringBuffer->dequeueAt(
					currentPtr, span.data() + progress, span.size() - progress);
			if(!success) {
				if(progress)
					break;
				co_await ringBuffer->wait(nextPtr);
				continue;
			}
			assert(actualSize); // For now, we do not support size zero records.
			if(actualSize == span.size() - progress) {
				if(progress)
					break;
				infoLogger() << "thor: Packet truncated on I/O channel "
						<< channel->descriptiveTag() << frg::endlog;
			}

			currentPtr = nextPtr;
			progress += actualSize;
		}

		channel->produceOutput(progress);
	}
}

} // namespace thor
