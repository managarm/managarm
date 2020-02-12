
#include <frigg/debug.hpp>
#include <arch/io_space.hpp>
#include "../../arch/x86/hpet.hpp"
#include "../../generic/fiber.hpp"
#include "../../generic/io.hpp"
#include "../../generic/kernel_heap.hpp"
#include "../../generic/service_helpers.hpp"
#include "rtc.hpp"
#include <clock.frigg_pb.hpp>
#include <mbus.frigg_pb.hpp>

namespace thor {

// TODO: Move this to a header file.
extern frigg::LazyInitializer<LaneHandle> mbusClient;

namespace {

arch::scalar_register<uint8_t> cmosIndex(0x70);
arch::scalar_register<uint8_t> cmosData(0x71);

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
//	frigg::infoLogger() << "thor: Waiting for RTC update in-progress" << frigg::endLog;
	while(!(readCmos(rtcStatusA) & 0x80))
		;
//	frigg::infoLogger() << "thor: Waiting for RTC update completion" << frigg::endLog;
	while(readCmos(rtcStatusA) & 0x80)
		frigg::pause();

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
	frigg::infoLogger() << "thor: Reading RTC returns " << y << "-" << mon << "-" << d
			<< " " << h << ":" << min << ":" << s << frigg::endLog;

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

bool handleReq(LaneHandle lane) {
	auto branch = fiberAccept(lane);
	if(!branch)
		return false;

	auto buffer = fiberRecv(branch);
	managarm::clock::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(buffer.data(), buffer.size());

	if(req.req_type() == managarm::clock::CntReqType::RTC_GET_TIME) {
		managarm::clock::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::clock::Error::SUCCESS);
		resp.set_ref_nanos(systemClockSource()->currentNanos());
		resp.set_time_nanos(getCmosTime());
	
		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		fiberSend(branch, ser.data(), ser.size());
	}else{
		managarm::clock::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::clock::Error::ILLEGAL_REQUEST);
		
		frg::string<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		fiberSend(branch, ser.data(), ser.size());
	}

	return true;
}

// ------------------------------------------------------------------------
// mbus object creation and management.
// ------------------------------------------------------------------------

LaneHandle createObject(LaneHandle mbus_lane) {
	auto branch = fiberOffer(mbus_lane);
	
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
	fiberSend(branch, ser.data(), ser.size());

	auto buffer = fiberRecv(branch);
	managarm::mbus::SvrResponse<KernelAlloc> resp(*kernelAlloc);
	resp.ParseFromArray(buffer.data(), buffer.size());
	assert(resp.error() == managarm::mbus::Error::SUCCESS);
	
	auto descriptor = fiberPullDescriptor(branch);
	assert(descriptor.is<LaneDescriptor>());
	return descriptor.get<LaneDescriptor>().handle;
}

void handleBind(LaneHandle object_lane) {
	auto branch = fiberAccept(object_lane);
	assert(branch);

	auto buffer = fiberRecv(branch);
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(buffer.data(), buffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);
	
	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frg::string<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	fiberSend(branch, ser.data(), ser.size());

	auto stream = createStream();
	fiberPushDescriptor(branch, LaneDescriptor{stream.get<1>()});

	// TODO: Do this in an own fiber.
	KernelFiber::run([lane = stream.get<0>()] () {
		while(true) {
			if(!handleReq(lane))
				break;
		}
	});
}

} // anonymous namespace

void initializeRtc() {
	// Create a fiber to manage requests to the RTC mbus object.
	KernelFiber::run([=] {
		auto object_lane = createObject(*mbusClient);
		while(true)
			handleBind(object_lane);
	});
}

} // namespace thor

