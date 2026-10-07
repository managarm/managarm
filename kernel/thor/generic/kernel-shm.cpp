#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <thor-internal/kernel-shm.hpp>

#include "shm.frigg_bragi.hpp"

namespace thor {

struct KernelRing {
	size_t size;
	smarter::shared_ptr<ImmediateMemory> producerMemory;
	smarter::shared_ptr<ImmediateMemory> consumerMemory;
	// The kernel keeps the raise end of the event of its own side and the wait end of the other.
	smarter::shared_ptr<SequencedEvent, TwoPeerPolicy> producerEvent;
	smarter::shared_ptr<SequencedEvent, TwoPeerPolicy> consumerEvent;
};

namespace {
	constexpr uint32_t writerRights = kHelRightRead | kHelRightWrite | kHelRightAssign;
	constexpr uint32_t readerRights = kHelRightRead | kHelRightAssign;

	// Never blocks; kicks that userspace did not handle yet are coalesced.
	void kick(SequencedEvent &event) {
		// This only fails if userspace is gone; it does not miss the kick in that case.
		(void)event.raise();
	}

	std::expected<KernelRing, Error> createRing(size_t size) {
		assert(protocols::shm::isValidRingSize(size) && !(size & (kPageSize - 1)));
		static_assert(protocols::shm::dataOffset == kPageSize);

		auto producerMemory = ImmediateMemory::create(protocols::shm::dataOffset + size);
		if(!producerMemory)
			return std::unexpected{producerMemory.error()};
		auto consumerMemory = ImmediateMemory::create(protocols::shm::consumerMemorySize);
		if(!consumerMemory)
			return std::unexpected{consumerMemory.error()};

		return KernelRing{
			.size = size,
			.producerMemory = std::move(*producerMemory),
			.consumerMemory = std::move(*consumerMemory),
			.producerEvent = {},
			.consumerEvent = {},
		};
	}

	bool matches(managarm::shm::EstablishRingRequest<KernelAlloc> &req,
			protocols::shm::Mode mode, protocols::shm::Framing framing, bool kernelProduces) {
		// The values of the protocols::shm enums match the bragi enums.
		return static_cast<int32_t>(req.mode()) == static_cast<int32_t>(mode)
				&& static_cast<int32_t>(req.framing()) == static_cast<int32_t>(framing)
				&& req.role() == (kernelProduces ? managarm::shm::Role::CONSUMER
						: managarm::shm::Role::PRODUCER);
	}

	// Pushes the memory and the kick event of one side of the ring. Keeps the end of the event
	// that the kernel needs: the raise end if the kernel owns the side, otherwise the wait end.
	coroutine<frg::expected<Error>> pushSide(smarter::shared_ptr<Stream, LanePolicy> conversation,
			smarter::shared_ptr<ImmediateMemory> memory,
			smarter::shared_ptr<SequencedEvent, TwoPeerPolicy> &event, bool remoteOwns) {
		auto memoryError = co_await pushDescriptor(conversation,
				AnyDescriptor::make<DescriptorType::memoryView>(std::move(memory),
					remoteOwns ? writerRights : readerRights));
		if(memoryError != Error::success)
			co_return memoryError;

		auto endsOutcome = SequencedEvent::create();
		if(!endsOutcome)
			co_return endsOutcome.error();
		auto waiter = std::move(endsOutcome->get<SequencedEvent::waitEnd>());
		auto raiser = std::move(endsOutcome->get<SequencedEvent::raiseEnd>());
		auto &pushedEnd = remoteOwns ? raiser : waiter;
		auto &keptEnd = remoteOwns ? waiter : raiser;
		event = std::move(keptEnd);
		auto eventError = co_await pushDescriptor(conversation,
				AnyDescriptor::make<DescriptorType::sequencedEvent>(std::move(pushedEnd),
					remoteOwns ? kHelRightSignal : kHelRightWait));
		if(eventError != Error::success)
			co_return eventError;
		co_return {};
	}

	coroutine<frg::expected<Error>> sendEstablishRingResponse(
			smarter::shared_ptr<Stream, LanePolicy> conversation,
			managarm::shm::Error error, size_t ringSize) {
		managarm::shm::EstablishRingResponse<KernelAlloc> resp{*kernelAlloc};
		resp.set_error(error);
		resp.set_ring_size(ringSize);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, resp.head_size};
		bragi::write_head_only(resp, respBuffer);
		auto respError = co_await sendBuffer(conversation, std::move(respBuffer));
		if(respError != Error::success)
			co_return respError;
		co_return {};
	}

	// Receives the EstablishRingRequest of a ring's shm handshake and checks it against the ring
	// that the kernel provides. On success, allocates the ring and answers with its size, then
	// pushes the memory and kick event of the producer and of the consumer.
	coroutine<frg::expected<Error, KernelRing>> provideRing(
			smarter::shared_ptr<Stream, LanePolicy> conversation,
			protocols::shm::Mode mode, protocols::shm::Framing framing,
			bool kernelProduces, size_t size) {
		auto [reqError, reqBuffer] = co_await recvBuffer(conversation);
		if(reqError != Error::success)
			co_return reqError;

		auto preamble = bragi::read_preamble(reqBuffer);
		if(preamble.error()
				|| preamble.id() != bragi::message_id<managarm::shm::EstablishRingRequest>)
			co_return Error::protocolViolation;
		auto req = bragi::parse_head_only<managarm::shm::EstablishRingRequest>(reqBuffer,
				*kernelAlloc);
		if(!req)
			co_return Error::protocolViolation;

		if(!matches(*req, mode, framing, kernelProduces)) {
			FRG_CO_TRY(co_await sendEstablishRingResponse(conversation,
					managarm::shm::Error::MISMATCH, 0));
			co_return Error::protocolViolation;
		}

		auto ringOutcome = createRing(size);
		if(!ringOutcome)
			co_return ringOutcome.error();
		auto ring = std::move(*ringOutcome);

		FRG_CO_TRY(co_await sendEstablishRingResponse(conversation,
				managarm::shm::Error::SUCCESS, size));

		FRG_CO_TRY(co_await pushSide(conversation, ring.producerMemory,
				ring.producerEvent, !kernelProduces));
		FRG_CO_TRY(co_await pushSide(conversation, ring.consumerMemory,
				ring.consumerEvent, kernelProduces));
		co_return std::move(ring);
	}
}

// ----------------------------------------------------------------------------
// KernelProducer.
// ----------------------------------------------------------------------------

KernelProducer::KernelProducer(KernelRing ring)
: producerWindow_{std::move(ring.producerMemory)},
		consumerWindow_{std::move(ring.consumerMemory)},
		core_{producerWindow_.access<protocols::shm::ProducerHeader>(0),
			consumerWindow_.access<protocols::shm::ConsumerHeader>(0),
			producerWindow_.bytes_data(protocols::shm::dataOffset), ring.size},
		event_{std::move(ring.producerEvent)},
		consumerEvent_{std::move(ring.consumerEvent)} { }

coroutine<frg::expected<Error, KernelProducer>> KernelProducer::provide(
		smarter::shared_ptr<Stream, LanePolicy> conversation,
		protocols::shm::Mode mode, protocols::shm::Framing framing, size_t size) {
	auto ring = FRG_CO_TRY(co_await provideRing(std::move(conversation),
			mode, framing, true, size));
	co_return KernelProducer{std::move(ring)};
}

void KernelProducer::produce(size_t size) {
	auto oldHead = core_.head();
	core_.produce(size);
	if(core_.needsKick(oldHead))
		kick(*event_);
}

void KernelProducer::flush() {
	if(core_.flush() && core_.needsKickAfterFlush())
		kick(*event_);
}

coroutine<frg::expected<Error>> KernelProducer::awaitSpace(size_t size) {
	while(true) {
		auto sequence = consumerEvent_->sequence();

		bool violation = false;
		bool wait = core_.armSpace(size, violation);
		if(violation)
			co_return Error::protocolViolation;
		if(!wait)
			co_return {};

		FRG_CO_TRY(co_await consumerEvent_->awaitEvent(sequence, {}));
	}
}

// ----------------------------------------------------------------------------
// KernelConsumer.
// ----------------------------------------------------------------------------

KernelConsumer::KernelConsumer(KernelRing ring)
: producerWindow_{std::move(ring.producerMemory)},
		consumerWindow_{std::move(ring.consumerMemory)},
		core_{producerWindow_.access<protocols::shm::ProducerHeader>(0),
			consumerWindow_.access<protocols::shm::ConsumerHeader>(0),
			producerWindow_.bytes_data(protocols::shm::dataOffset), ring.size},
		producerEvent_{std::move(ring.producerEvent)},
		event_{std::move(ring.consumerEvent)} { }

coroutine<frg::expected<Error, KernelConsumer>> KernelConsumer::provide(
		smarter::shared_ptr<Stream, LanePolicy> conversation,
		protocols::shm::Mode mode, protocols::shm::Framing framing, size_t size) {
	auto ring = FRG_CO_TRY(co_await provideRing(std::move(conversation),
			mode, framing, false, size));
	co_return KernelConsumer{std::move(ring)};
}

void KernelConsumer::consume(size_t size) {
	if(core_.consume(size))
		kick(*event_);
}

coroutine<frg::expected<Error>> KernelConsumer::awaitData(size_t watermark) {
	while(true) {
		auto sequence = producerEvent_->sequence();

		bool violation = false;
		bool wait = core_.armData(watermark, violation);
		if(violation)
			co_return Error::protocolViolation;
		if(!wait)
			co_return {};

		FRG_CO_TRY(co_await producerEvent_->awaitEvent(sequence, {}));
	}
}

} // namespace thor
