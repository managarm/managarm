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
#include <thor-internal/mbus.hpp>

// --------------------------------------------------------------------------------------
// Core ostrace implementation.
// --------------------------------------------------------------------------------------

namespace thor {

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

struct OstraceBusObject : private KernelBusObject {
	coroutine<void> run() {
		Properties properties;
		properties.stringProperty("class", frg::string<KernelAlloc>(*kernelAlloc, "ostrace"));

		// TODO(qookie): Better error handling here.
		(co_await createObject("ostrace", std::move(properties))).unwrap();
	}

private:
	coroutine<frg::expected<Error>> handleRequest(LaneHandle boundLane) override {
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

		co_return frg::success;
	}
};

initgraph::Task initOsTraceMbus{&globalInitEngine, "generic.init-ostrace-sinks",
	initgraph::Requires{&initOsTraceCore,
		getFibersAvailableStage(),
		getIoChannelsDiscoveredStage()},
	[] {
		// Create a fiber to manage requests to the ostrace mbus object.
		KernelFiber::run([=] {
			// We unconditionally create the mbus object since userspace might use it.
			auto ostrace = frg::construct<OstraceBusObject>(*kernelAlloc);
			async::detach_with_allocator(*kernelAlloc, ostrace->run());

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
