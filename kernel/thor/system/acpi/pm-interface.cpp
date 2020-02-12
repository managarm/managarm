
#include <frigg/debug.hpp>
#include <arch/io_space.hpp>
#include "../../arch/x86/hpet.hpp"
#include "../../generic/fiber.hpp"
#include "../../generic/io.hpp"
#include "../../generic/kernel_heap.hpp"
#include "../../generic/service_helpers.hpp"
#include "pm-interface.hpp"
#include <hw.frigg_pb.hpp>
#include <mbus.frigg_pb.hpp>

#include <lai/helpers/pm.h>

namespace thor {
	// TODO: Move this to a header file.
	extern frigg::LazyInitializer<LaneHandle> mbusClient;
}

namespace thor::acpi {

arch::scalar_register<uint8_t> ps2Command(0x64);

constexpr uint8_t ps2Reset = 0xFE;

void issuePs2Reset() {
	arch::io_space space;
	space.store(ps2Command, ps2Reset);
	pollSleepNano(100'000'000); // 100ms should be long enough to actually reset.
}

namespace {

bool handleReq(LaneHandle lane) {
	auto branch = fiberAccept(lane);
	if(!branch)
		return false;

	auto buffer = fiberRecv(branch);
	managarm::hw::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(buffer.data(), buffer.size());

	if(req.req_type() == managarm::hw::CntReqType::PM_RESET) {
		if(lai_acpi_reset())
			frigg::infoLogger() << "thor: ACPI reset failed" << frigg::endLog;

		issuePs2Reset();
		frigg::infoLogger() << "thor: Reset using PS/2 controller failed" << frigg::endLog;

		frigg::panicLogger() << "thor: We do not know how to reset" << frigg::endLog;
	}else{
		managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::hw::Errors::ILLEGAL_REQUEST);

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
	cls_item.set_value(frg::string<KernelAlloc>(*kernelAlloc, "pm-interface"));

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

void initializePmInterface() {
	// Create a fiber to manage requests to the RTC mbus object.
	KernelFiber::run([=] {
		auto object_lane = createObject(*mbusClient);
		while(true)
			handleBind(object_lane);
	});
}

} // namespace thor::acpi
