#include <thor-internal/debug.hpp>
#include <thor-internal/user-io.hpp>

namespace thor {

namespace {
	using protocols::shm::Framing;
	using protocols::shm::Mode;
}

UserIoChannel::UserIoChannel(CtorToken, frg::string<KernelAlloc> tag,
		frg::string<KernelAlloc> descriptiveTag,
		KernelProducer out, frg::optional<KernelConsumer> in)
: KernelIoChannel{std::move(tag), std::move(descriptiveTag)},
		out_{std::move(out)}, in_{std::move(in)} {
	updateSpans_();
}

coroutine<frg::expected<Error>> UserIoChannel::provide(
		smarter::shared_ptr<Stream, LanePolicy> conversation,
		frg::string<KernelAlloc> tag, frg::string<KernelAlloc> descriptiveTag, bool input) {
	// See ProvideIoChannelRequest for the order of the rings.
	auto out = FRG_CO_TRY(co_await KernelProducer::provide(conversation,
			Mode::reliable, Framing::stream, outRingSize));
	frg::optional<KernelConsumer> in;
	if(input)
		in = FRG_CO_TRY(co_await KernelConsumer::provide(std::move(conversation),
				Mode::reliable, Framing::stream, inRingSize));

	auto channel = smarter::allocate_shared<UserIoChannel>(*kernelAlloc, CtorToken{},
			std::move(tag), std::move(descriptiveTag), std::move(out), std::move(in));

	infoLogger() << "thor: Registered user I/O channel "
			<< channel->descriptiveTag() << frg::endlog;
	publishIoChannel(std::move(channel));
	co_return {};
}

void UserIoChannel::updateSpans_() {
	updateWritableSpan(out_.writableSpan());
	if(in_)
		updateReadableSpan(in_->readableSpan());
}

void UserIoChannel::produceOutput(size_t n) {
	out_.produce(n);
	updateSpans_();
}

void UserIoChannel::consumeInput(size_t n) {
	in_->consume(n);
	updateSpans_();
}

coroutine<frg::expected<Error>> UserIoChannel::issueIo(IoFlags flags) {
	if((flags & ioProgressInput) && !in_)
		co_return Error::illegalState;
	if((flags & ioProgressOutput) && (flags & ioProgressInput)) {
		// Each ring has its own kick event; no user needs to wait for both so far.
		warningLogger() << "thor: User I/O channel " << descriptiveTag()
				<< " cannot await progress in both directions" << frg::endlog;
		co_return Error::illegalArgs;
	}

	// The server only consumes output once it is written out, so wait until the ring drains.
	if(flags & ioFlush) {
		out_.flush();
		FRG_CO_TRY(co_await out_.awaitSpace(out_.size()));
	}

	// Progress means that the span grows beyond its current size.
	// The span cannot grow if the output ring is already empty (or the input ring full).
	// Waiting fails with endOfLane if the server is gone.
	if(flags & ioProgressOutput) {
		auto target = out_.freeSize() + 1;
		if(target <= out_.size())
			FRG_CO_TRY(co_await out_.awaitSpace(target));
	}else if(flags & ioProgressInput) {
		auto target = in_->availableSize() + 1;
		if(target <= in_->size())
			FRG_CO_TRY(co_await in_->awaitData(target));
	}
	updateSpans_();
	co_return {};
}

} // namespace thor
