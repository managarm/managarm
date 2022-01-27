#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/small_vector.hpp>
#include <frg/span.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/kernel-io.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/ostrace.hpp>
#include <thor-internal/stream.hpp>
#include <thor-internal/timer.hpp>
#include <mbus.frigg_pb.hpp>

// --------------------------------------------------------------------------------------
// Core ostrace implementation.
// --------------------------------------------------------------------------------------

namespace thor {

extern frg::manual_box<LaneHandle> mbusClient;

bool wantOsTrace = false;

constinit std::atomic<bool> osTraceInUse{false};

initgraph::Stage *getOsTraceAvailableStage() {
	static initgraph::Stage s{&globalInitEngine, "generic.ostrace-available"};
	return &s;
}

namespace {

std::atomic<uint64_t> nextId{1};
frg::manual_box<LogRingBuffer> globalOsTraceRing;

initgraph::Task initOsTraceCore{&globalInitEngine, "generic.init-ostrace-core",
	initgraph::Entails{getOsTraceAvailableStage()},
	[] {
		if(!wantOsTrace)
			return;

		void *osTraceMemory = kernelAlloc->allocate(1 << 20);
		globalOsTraceRing.initialize(reinterpret_cast<uintptr_t>(osTraceMemory), 1 << 20);

		osTraceInUse.store(true);
	}
};

template<typename R>
void commitOsTrace(R record) {
	if(!osTraceInUse.load(std::memory_order_relaxed))
		return;

	auto ts = record.size_of_tail();
	frg::small_vector<char, 64, KernelAlloc> ser(*kernelAlloc);
	ser.resize(8 + ts);
	bool encodeSuccess = bragi::write_head_tail(record,
			frg::span<char>(ser.data(), 8),
			frg::span<char>(ser.data() + 8, ts));
	assert(encodeSuccess);

	// We want to be able to call this function from any context, but we cannot wake the waiters
	// in all contexts. For now, only wake waiters if IRQs are enabled.
	globalOsTraceRing->enqueue(ser.data(), ser.size(), !intsAreEnabled());
}

} // anonymous namespace

OsTraceEventId announceOsTraceEvent(frg::string_view name) {
	auto id = nextId.fetch_add(1, std::memory_order_relaxed);

	managarm::ostrace::AnnounceEventRecord<KernelAlloc> record{*kernelAlloc};
	record.set_id(id);
	record.set_name(frg::string<KernelAlloc>{*kernelAlloc, name});
	commitOsTrace(std::move(record));

	return static_cast<OsTraceEventId>(id);
}

void emitOsTrace(managarm::ostrace::EventRecord<KernelAlloc> record) {
	record.set_ts(systemClockSource()->currentNanos());

	commitOsTrace(std::move(record));
}

LogRingBuffer *getGlobalOsTraceRing() {
	return globalOsTraceRing.get();
}

// --------------------------------------------------------------------------------------
// mbus object handling.
// --------------------------------------------------------------------------------------

namespace {

coroutine<void> handleBind(LaneHandle objectLane);
coroutine<Error> handleReq(LaneHandle boundLane);

coroutine<void> createObject(LaneHandle mbusLane) {
	auto [offerError, lane] = co_await OfferSender{mbusLane};
	assert(offerError == Error::success && "Unexpected mbus transaction");

	managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
	cls_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "class"));
	auto &cls_item = cls_prop.mutable_item().mutable_string_item();
	cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "ostrace"));

	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(cls_prop));

	frg::string<KernelAlloc> ser(*kernelAlloc);
	req.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
	memcpy(reqBuffer.data(), ser.data(), ser.size());
	auto reqError = co_await SendBufferSender{lane, std::move(reqBuffer)};
	assert(reqError == Error::success && "Unexpected mbus transaction");

	auto [respError, respBuffer] = co_await RecvBufferSender{lane};
	assert(respError == Error::success && "Unexpected mbus transaction");
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(respBuffer.data(), respBuffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	auto [objectError, objectDescriptor] = co_await PullDescriptorSender{lane};
	assert(objectError == Error::success && "Unexpected mbus transaction");
	assert(objectDescriptor.is<LaneDescriptor>());
	auto objectLane = objectDescriptor.get<LaneDescriptor>().handle;
	while(true)
		co_await handleBind(objectLane);
}

coroutine<void> handleBind(LaneHandle objectLane) {
	auto [acceptError, lane] = co_await AcceptSender{objectLane};
	assert(acceptError == Error::success && "Unexpected mbus transaction");

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	assert(reqError == Error::success && "Unexpected mbus transaction");
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);

	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::string<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
	memcpy(respBuffer.data(), ser.data(), ser.size());
	auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
	assert(respError == Error::success && "Unexpected mbus transaction");

	auto stream = createStream();
	auto boundError = co_await PushDescriptorSender{lane, LaneDescriptor{stream.get<1>()}};
	assert(boundError == Error::success && "Unexpected mbus transaction");
	auto boundLane = stream.get<0>();

	async::detach_with_allocator(*kernelAlloc, ([] (LaneHandle boundLane) -> coroutine<void> {
		while(true) {
			auto error = co_await handleReq(boundLane);
			if(error == Error::endOfLane)
				break;
			if(error == Error::protocolViolation) {
				infoLogger() << "thor: Aborting ostrace request"
						" after remote violated the protocol" << frg::endlog;
			}else{
				assert(error == Error::success);
			}
		}
	})(boundLane));
}

coroutine<Error> handleReq(LaneHandle boundLane) {
	auto [acceptError, lane] = co_await AcceptSender{boundLane};
	if(acceptError == Error::endOfLane)
		co_return Error::endOfLane;
	if(acceptError != Error::success) {
		assert(isRemoteIpcError(acceptError));
		co_return Error::protocolViolation;
	}

	auto [reqError, reqBuffer] = co_await RecvBufferSender{lane};
	if(reqError != Error::success) {
		assert(isRemoteIpcError(reqError));
		co_return Error::protocolViolation;
	}
	frg::span<const char> reqSpan{reinterpret_cast<const char *>(reqBuffer.data()),
			reqBuffer.size()};

	auto preamble = bragi::read_preamble(reqSpan);
	if(preamble.error())
		co_return Error::protocolViolation;

	// All records have a head size of 128.
	auto headSpan = reqSpan.subspan(0, 128);
	auto tailSpan = reqSpan.subspan(128, preamble.tail_size());

	switch (preamble.id()) {
	case bragi::message_id<managarm::ostrace::NegotiateReq>: {
		auto maybeReq = bragi::parse_head_tail<managarm::ostrace::NegotiateReq>(
				headSpan, tailSpan, *kernelAlloc);
		if(!maybeReq)
			co_return Error::protocolViolation;

		managarm::ostrace::Response<KernelAlloc> resp(*kernelAlloc);
		if(wantOsTrace) {
			resp.set_error(managarm::ostrace::Error::SUCCESS);
		}else{
			resp.set_error(managarm::ostrace::Error::OSTRACE_GLOBALLY_DISABLED);
		}

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		if(respError != Error::success) {
			assert(isRemoteIpcError(respError));
			co_return Error::protocolViolation;
		}
	} break;
	case bragi::message_id<managarm::ostrace::EmitEventReq>: {
		auto maybeReq = bragi::parse_head_tail<managarm::ostrace::EmitEventReq>(
				headSpan, tailSpan, *kernelAlloc);
		if(!maybeReq)
			co_return Error::protocolViolation;
		auto &req = maybeReq.value();

		managarm::ostrace::EventRecord<KernelAlloc> record{*kernelAlloc};
		record.set_id(req.id());
		for(size_t i = 0; i < req.ctrs_size(); ++i)
			record.add_ctrs(std::move(req.ctrs(i)));
		emitOsTrace(std::move(record));

		managarm::ostrace::Response<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::ostrace::Error::SUCCESS);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		if(respError != Error::success) {
			assert(isRemoteIpcError(respError));
			co_return Error::protocolViolation;
		}
	} break;
	case bragi::message_id<managarm::ostrace::AnnounceEventReq>: {
		auto maybeReq = bragi::parse_head_tail<managarm::ostrace::AnnounceEventReq>(
				headSpan, tailSpan, *kernelAlloc);
		if(!maybeReq)
			co_return Error::protocolViolation;
		auto &req = maybeReq.value();

		auto id = nextId.fetch_add(1, std::memory_order_relaxed);

		managarm::ostrace::AnnounceEventRecord<KernelAlloc> record{*kernelAlloc};
		record.set_id(id);
		record.set_name(std::move(req.name()));
		commitOsTrace(std::move(record));

		managarm::ostrace::Response<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::ostrace::Error::SUCCESS);
		resp.set_id(id);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		if(respError != Error::success) {
			assert(isRemoteIpcError(respError));
			co_return Error::protocolViolation;
		}
	} break;
	case bragi::message_id<managarm::ostrace::AnnounceItemReq>: {
		auto maybeReq = bragi::parse_head_tail<managarm::ostrace::AnnounceItemReq>(
				headSpan, tailSpan, *kernelAlloc);
		if(!maybeReq)
			co_return Error::protocolViolation;
		auto &req = maybeReq.value();

		auto id = nextId.fetch_add(1, std::memory_order_relaxed);

		managarm::ostrace::AnnounceItemRecord<KernelAlloc> record{*kernelAlloc};
		record.set_id(id);
		record.set_name(std::move(req.name()));
		commitOsTrace(std::move(record));

		managarm::ostrace::Response<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::ostrace::Error::SUCCESS);
		resp.set_id(id);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		if(respError != Error::success) {
			assert(isRemoteIpcError(respError));
			co_return Error::protocolViolation;
		}
	} break;
	default:
		managarm::ostrace::Response<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::ostrace::Error::ILLEGAL_REQUEST);

		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{lane, std::move(respBuffer)};
		if(respError != Error::success) {
			assert(isRemoteIpcError(respError));
			co_return Error::protocolViolation;
		}
	}

	co_return Error::success;
}

initgraph::Task initOsTraceMbus{&globalInitEngine, "generic.init-ostrace-sinks",
	initgraph::Requires{&initOsTraceCore,
		getFibersAvailableStage(),
		getIoChannelsDiscoveredStage()},
	[] {
		// Create a fiber to manage requests to the ostrace mbus object.
		KernelFiber::run([=] {
			// We unconditionally create the mbus object since userspace might use it.
			async::detach_with_allocator(*kernelAlloc, createObject(*mbusClient));

			// Only dump to an I/O channel if ostrace is supported (otherwise, the ring buffer
			// does not even exist).
			if(wantOsTrace) {
				auto channel = solicitIoChannel("ostrace");
				if(channel) {
					infoLogger() << "thor: Connecting ostrace to I/O channel" << frg::endlog;
					async::detach_with_allocator(*kernelAlloc,
							dumpRingToChannel(globalOsTraceRing.get(), std::move(channel), 256));
				}
			}
		});
	}
};

} // anonymous namespace

} // namespace thor
