#include <bragi/helpers-all.hpp>
#include <bragi/helpers-frigg.hpp>
#include <frg/cmdline.hpp>
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
		frg::array args = {
			frg::option{"ostrace", frg::store_true(wantOsTrace)},
		};
		frg::parse_arguments(getKernelCmdline(), args);

		infoLogger() << "thor: ostrace is " << (wantOsTrace ? "enabled" : "disabled") << frg::endlog;
		if(!wantOsTrace)
			return;

		void *osTraceMemory = kernelAlloc->allocate(1 << 20);
		globalOsTraceRing.initialize(reinterpret_cast<uintptr_t>(osTraceMemory), 1 << 20);

		osTraceInUse.store(true);

		ostrace::setup();
	}
};

void doEmit(frg::span<char> payload) {
	if(!osTraceInUse.load(std::memory_order_relaxed))
		return;

	struct Header {
		uint32_t size;
	};

	frg::small_vector<char, 64, KernelAlloc> buffer{*kernelAlloc};
	buffer.resize(sizeof(Header) + payload.size());

	auto hdr = new (buffer.data()) Header;
	hdr->size = payload.size();

	memcpy(buffer.data() + sizeof(Header), payload.data(), payload.size());

	// We want to be able to call this function from any context, but we cannot wake the waiters
	// in all contexts. For now, only wake waiters if IRQs are enabled.
	globalOsTraceRing->enqueue(buffer.data(), buffer.size(), !intsAreEnabled());
}

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

	doEmit({ser.data(), ser.size()});
}

} // anonymous namespace

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

		switch (preamble.id()) {
		case bragi::message_id<managarm::ostrace::NegotiateReq>: {
			auto maybeReq = bragi::parse_head_only<managarm::ostrace::NegotiateReq>(
					reqSpan, *kernelAlloc);
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
		case bragi::message_id<managarm::ostrace::EmitReq>: {
			auto maybeReq = bragi::parse_head_only<managarm::ostrace::EmitReq>(
					reqSpan, *kernelAlloc);
			if(!maybeReq)
				co_return Error::protocolViolation;
			//auto &req = maybeReq.value();

			auto [dataError, dataBuffer] = co_await RecvBufferSender{lane};
			if(dataError != Error::success) {
				assert(isRemoteIpcError(dataError));
				co_return Error::protocolViolation;
			}

			doEmit({reinterpret_cast<char *>(dataBuffer.data()), dataBuffer.size()});

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
		case bragi::message_id<managarm::ostrace::AnnounceItemReq>: {
			auto maybeReq = bragi::parse_head_only<managarm::ostrace::AnnounceItemReq>(
					reqSpan, *kernelAlloc);
			if(!maybeReq)
				co_return Error::protocolViolation;
			auto &req = maybeReq.value();

			auto id = nextId.fetch_add(1, std::memory_order_relaxed);

			managarm::ostrace::Definition<KernelAlloc> record{*kernelAlloc};
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

// --------------------------------------------------------------------------------------
// Kernel ostrace infrastructure.
// --------------------------------------------------------------------------------------

namespace ostrace {

std::atomic<bool> available{false};

THOR_DEFINE_PERCPU(context);

void setup() {
	auto setupTerm = [] (ostrace::Term &term) {
		assert(!term.id_);
		term.id_ = nextId.fetch_add(1, std::memory_order_relaxed);

		managarm::ostrace::Definition<KernelAlloc> record{*kernelAlloc};
		record.set_id(term.id_);
		record.set_name(frg::string<KernelAlloc>{*kernelAlloc, term.name_});
		commitOsTrace(std::move(record));
	};

	setupTerm(ostEvtArmPreemption);
	setupTerm(ostEvtArmCpuTimer);
	available.store(true, std::memory_order_relaxed);
}

void emitBuffer(frg::span<char> payload) {
	doEmit(payload);
}

} // namespace ostrace

// --------------------------------------------------------------------------------------
// Kernel ostrace events.
// --------------------------------------------------------------------------------------

ostrace::Event ostEvtArmPreemption{"thor.arm-preemption"};
ostrace::Event ostEvtArmCpuTimer{"thor.arm-cpu-timer"};

} // namespace thor
