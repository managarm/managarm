#include <arch/io_space.hpp>
#include <thor-internal/mbus.hpp>
#include <thor-internal/arch/hpet.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/stream.hpp>
#include <bragi/helpers-frigg.hpp>
#include <bragi/helpers-all.hpp>
#include <clock.frigg_bragi.hpp>

namespace thor {

namespace {

inline constexpr arch::scalar_register<uint8_t> cmosIndex(0x70);
inline constexpr arch::scalar_register<uint8_t> cmosData(0x71);

constexpr unsigned int rtcSeconds = 0x00;
constexpr unsigned int rtcMinutes = 0x02;
constexpr unsigned int rtcHours = 0x04;
constexpr unsigned int rtcDay = 0x07;
constexpr unsigned int rtcMonth = 0x08;
constexpr unsigned int rtcYear = 0x09;
constexpr unsigned int rtcStatusA = 0x0A;
constexpr unsigned int rtcStatusB = 0x0B;

uint8_t readCmos(unsigned int offset) {
	arch::io_space space;
	space.store(cmosIndex, offset);
	return space.load(cmosData);
}

int64_t getCmosTime() {
	const uint64_t nanoPrefix = 1e9;
	
	// Wait until the RTC update-in-progress bit gets set and reset.
	// TODO: fiberSleep(1'000) does not seem to work here.
//	infoLogger() << "thor: Waiting for RTC update in-progress" << frg::endlog;
	while(!(readCmos(rtcStatusA) & 0x80))
		;
//	infoLogger() << "thor: Waiting for RTC update completion" << frg::endlog;
	while(readCmos(rtcStatusA) & 0x80)
		pause();

	// Perform the actual RTC read.
	bool status_b = readCmos(rtcStatusB);

	auto decodeRtc = [&] (uint8_t raw) -> int64_t {
		if(!(status_b & 0x04))
			return (raw >> 4) * 10 + (raw & 0x0F);
		return raw;
	};

	assert(!(status_b & 0x02)); // 24 hour format.
	
	int64_t d = decodeRtc(readCmos(rtcDay));
	int64_t mon = decodeRtc(readCmos(rtcMonth));
	int64_t y = decodeRtc(readCmos(rtcYear)) + 2000; // TODO: Use century register.
	int64_t s = decodeRtc(readCmos(rtcSeconds));
	int64_t min = decodeRtc(readCmos(rtcMinutes));
	int64_t h = decodeRtc(readCmos(rtcHours));
	infoLogger() << "thor: Reading RTC returns " << y << "-" << mon << "-" << d
			<< " " << h << ":" << min << ":" << s << frg::endlog;

	// Code from http://howardhinnant.github.io/date_algorithms.html
	y -= (mon <= 2);
	const int64_t era = (y >= 0 ? y : y - 399) / 400;
	unsigned int yoe = static_cast<unsigned int>(y - era * 400);  // [0, 399]
	unsigned int doy = (153 * (mon + (mon > 2 ? -3 : 9)) + 2)/5 + d - 1;  // [0, 365]
	unsigned int doe = yoe * 365 + yoe/4 - yoe/100 + doy;         // [0, 146096]
	int64_t days = era * 146097 + static_cast<int64_t>(doe) - 719468;

	return s * nanoPrefix + min * 60 * nanoPrefix + h * 3600 * nanoPrefix
			+ days * 86400 * nanoPrefix;
}

struct RtcBusObject : private KernelBusObject {
	coroutine<void> run() {
		Properties properties;
		properties.stringProperty("class", frg::string<KernelAlloc>(*kernelAlloc, "rtc"));

		// TODO(qookie): Better error handling here.
		(co_await createObject("legacy-pc/rtc", std::move(properties))).unwrap();
	}

private:
	coroutine<frg::expected<Error>> handleRequest(LaneHandle lane) override {
		auto [acceptError, conversation] = co_await AcceptSender{lane};
		if(acceptError != Error::success)
			co_return acceptError;

		auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
		if(reqError != Error::success)
			co_return reqError;

		auto preamble = bragi::read_preamble(reqBuffer);
		if (preamble.error())
			co_return Error::protocolViolation;

		auto sendResponse = [] (LaneHandle &conversation,
				managarm::clock::SvrResponse<KernelAlloc> &&resp) -> coroutine<frg::expected<Error>> {
			frg::unique_memory<KernelAlloc> respHeadBuffer{*kernelAlloc,
				resp.head_size};

			frg::unique_memory<KernelAlloc> respTailBuffer{*kernelAlloc,
				resp.size_of_tail()};

			bragi::write_head_tail(resp, respHeadBuffer, respTailBuffer);

			auto respHeadError = co_await SendBufferSender{conversation, std::move(respHeadBuffer)};

			if (respHeadError != Error::success)
				co_return respHeadError;

			auto respTailError = co_await SendBufferSender{conversation, std::move(respTailBuffer)};

			if (respTailError != Error::success)
				co_return respTailError;

			co_return frg::success;
		};

		if(preamble.id() == bragi::message_id<managarm::clock::GetRtcTimeRequest>) {
			managarm::clock::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::clock::Error::SUCCESS);
			resp.set_ref_nanos(systemClockSource()->currentNanos());
			resp.set_rtc_nanos(getCmosTime());

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
		} else {
			managarm::clock::SvrResponse<KernelAlloc> resp(*kernelAlloc);
			resp.set_error(managarm::clock::Error::ILLEGAL_REQUEST);

			FRG_CO_TRY(co_await sendResponse(conversation, std::move(resp)));
		}

		co_return frg::success;
	}
};


} // anonymous namespace

static initgraph::Task initRtcTask{&globalInitEngine, "x86.init-rtc",
	initgraph::Requires{getFibersAvailableStage()},
	[] {
		// Create a fiber to manage requests to the RTC mbus object.
		KernelFiber::run([=] {
			auto rtc = frg::construct<RtcBusObject>(*kernelAlloc);
			async::detach_with_allocator(*kernelAlloc, rtc->run());
		});
	}
};

} // namespace thor

