#include <arch/io_space.hpp>
#include <thor-internal/arch/hpet.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/main.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <clock.frigg_pb.hpp>
#include <mbus.frigg_pb.hpp>

namespace thor {

// TODO: Move this to a header file.
extern frg::manual_box<LaneHandle> mbusClient;

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

coroutine<bool> handleReq(LaneHandle lane) {
	auto [acceptError, conversation] = co_await AcceptSender{lane};
	if(acceptError == Error::endOfLane)
		co_return false;
	// TODO: improve error handling here.
	assert(acceptError == Error::success);

	auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
	// TODO: improve error handling here.
	assert(reqError == Error::success);

	managarm::clock::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());

	if(req.req_type() == managarm::clock::CntReqType::RTC_GET_TIME) {
		managarm::clock::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::clock::Error::SUCCESS);
		resp.set_ref_nanos(systemClockSource()->currentNanos());
		resp.set_time_nanos(getCmosTime());
	
		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
		// TODO: improve error handling here.
		assert(respError == Error::success);
	}else{
		managarm::clock::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::clock::Error::ILLEGAL_REQUEST);
		
		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
		memcpy(respBuffer.data(), ser.data(), ser.size());
		auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
		// TODO: improve error handling here.
		assert(respError == Error::success);
	}

	co_return true;
}

// ------------------------------------------------------------------------
// mbus object creation and management.
// ------------------------------------------------------------------------

coroutine<LaneHandle> createObject(LaneHandle mbusLane) {
	auto [offerError, conversation] = co_await OfferSender{mbusLane};
	// TODO: improve error handling here.
	assert(offerError == Error::success);
	
	managarm::mbus::Property<KernelAlloc> cls_prop(*kernelAlloc);
	cls_prop.set_name(frg::string<KernelAlloc>(*kernelAlloc, "class"));
	auto &cls_item = cls_prop.mutable_item().mutable_string_item();
	cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "rtc"));
	
	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(cls_prop));

	frg::string<KernelAlloc> ser(*kernelAlloc);
	req.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> reqBuffer{*kernelAlloc, ser.size()};
	memcpy(reqBuffer.data(), ser.data(), ser.size());
	auto reqError = co_await SendBufferSender{conversation, std::move(reqBuffer)};
	// TODO: improve error handling here.
	assert(reqError == Error::success);

	auto [respError, respBuffer] = co_await RecvBufferSender{conversation};
	// TODO: improve error handling here.
	assert(respError == Error::success);
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(respBuffer.data(), respBuffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);

	auto [descError, descriptor] = co_await PullDescriptorSender{conversation};
	// TODO: improve error handling here.
	assert(descError == Error::success);
	assert(descriptor.is<LaneDescriptor>());
	co_return descriptor.get<LaneDescriptor>().handle;
}

coroutine<void> handleBind(LaneHandle objectLane) {
	auto [acceptError, conversation] = co_await AcceptSender{objectLane};
	// TODO: improve error handling here.
	assert(acceptError == Error::success);

	auto [reqError, reqBuffer] = co_await RecvBufferSender{conversation};
	// TODO: improve error handling here.
	assert(reqError == Error::success);
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(reqBuffer.data(), reqBuffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);

	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::string<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	frg::unique_memory<KernelAlloc> respBuffer{*kernelAlloc, ser.size()};
	memcpy(respBuffer.data(), ser.data(), ser.size());
	auto respError = co_await SendBufferSender{conversation, std::move(respBuffer)};
	// TODO: improve error handling here.
	assert(respError == Error::success);

	auto stream = createStream();
	auto descError = co_await PushDescriptorSender{conversation,
			LaneDescriptor{stream.get<1>()}};
	// TODO: improve error handling here.
	assert(descError == Error::success);

	async::detach_with_allocator(*kernelAlloc, [] (LaneHandle lane) -> coroutine<void> {
		while(true) {
			if(!(co_await handleReq(lane)))
				break;
		}
	}(std::move(stream.get<0>())));
}

} // anonymous namespace

static initgraph::Task initRtcTask{&globalInitEngine, "x86.init-rtc",
	initgraph::Requires{getTaskingAvailableStage()},
	[] {
		// Create a fiber to manage requests to the RTC mbus object.
		KernelFiber::run([=] {
			async::detach_with_allocator(*kernelAlloc, [] () -> coroutine<void> {
				auto objectLane = co_await createObject(*mbusClient);
				while(true)
					co_await handleBind(objectLane);
			}());
		});
	}
};

} // namespace thor

