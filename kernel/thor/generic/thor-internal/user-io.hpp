#pragma once

#include <frg/optional.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/kernel-shm.hpp>

namespace thor {

// KernelIoChannel whose transport is a userspace server. Output and input travel through
// rings of the shm protocol; the server establishes them via kerncfg's ProvideIoChannel.
struct UserIoChannel final : KernelIoChannel {
private:
	struct CtorToken {};

public:
	static constexpr size_t outRingSize = size_t{64} << 10;
	static constexpr size_t inRingSize = size_t{4} << 10;

	// Establishes the rings of a new channel over the conversation lane and publishes the channel.
	static coroutine<frg::expected<Error>> provide(
			smarter::shared_ptr<Stream, LanePolicy> conversation,
			frg::string<KernelAlloc> tag, frg::string<KernelAlloc> descriptiveTag, bool input);

	UserIoChannel(CtorToken, frg::string<KernelAlloc> tag,
			frg::string<KernelAlloc> descriptiveTag,
			KernelProducer out, frg::optional<KernelConsumer> in);

	void produceOutput(size_t n) override;
	void consumeInput(size_t n) override;
	coroutine<frg::expected<Error>> issueIo(IoFlags flags) override;

private:
	void updateSpans_();

	// Output is produced by the kernel and consumed by the server, input vice versa.
	KernelProducer out_;
	frg::optional<KernelConsumer> in_;
};

} // namespace thor
