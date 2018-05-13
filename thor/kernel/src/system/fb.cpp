
#include <frigg/debug.hpp>
#include <arch/io_space.hpp>
#include "../arch/x86/hpet.hpp"
#include "../generic/fiber.hpp"
#include "../generic/io.hpp"
#include "../generic/kernel_heap.hpp"
#include "../generic/service_helpers.hpp"
#include "fb.hpp"
#include <mbus.frigg_pb.hpp>
#include <hw.frigg_pb.hpp>

namespace thor {

// TODO: Move this to a header file.
extern frigg::LazyInitializer<LaneHandle> mbusClient;

namespace {

struct FbInfo {
	uint64_t address;
	uint64_t pitch;
	uint64_t width;
	uint64_t height;
	uint64_t bpp;
	uint64_t type;
	
	frigg::SharedPtr<Memory> memory;
};

bool handleReq(LaneHandle lane, FbInfo *info) {
	auto branch = fiberAccept(lane);
	if(!branch)
		return false;

	auto buffer = fiberRecv(branch);
	managarm::hw::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(buffer.data(), buffer.size());

	if(req.req_type() == managarm::hw::CntReqType::GET_FB_INFO) {
		managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::hw::Errors::SUCCESS);
		resp.set_fb_pitch(info->pitch);
		resp.set_fb_width(info->width);
		resp.set_fb_height(info->height);
		resp.set_fb_bpp(info->bpp);
		resp.set_fb_type(info->type);
	
		frigg::String<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		fiberSend(branch, ser.data(), ser.size());
	}else if(req.req_type() == managarm::hw::CntReqType::ACCESS_BAR) {
		AnyDescriptor descriptor;
		assert(device->bars[index].type == PciDevice::kBarMemory);
		descriptor = MemoryAccessDescriptor{info->memory};
		
		managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::hw::Errors::SUCCESS);
	
		frigg::String<KernelAlloc> ser(*kernelAlloc);
		resp.SerializeToString(&ser);
		fiberSend(branch, ser.data(), ser.size());
		fiberPushDescriptor(branch, descriptor);
	}else{
		managarm::hw::SvrResponse<KernelAlloc> resp(*kernelAlloc);
		resp.set_error(managarm::hw::Errors::ILLEGAL_REQUEST);
		
		frigg::String<KernelAlloc> ser(*kernelAlloc);
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
	cls_prop.set_name(frigg::String<KernelAlloc>(*kernelAlloc, "class"));
	auto &cls_item = cls_prop.mutable_item().mutable_string_item();
	cls_item.set_value(frigg::String<KernelAlloc>(*kernelAlloc, "framebuffer"));
	
	managarm::mbus::CntRequest<KernelAlloc> req(*kernelAlloc);
	req.set_req_type(managarm::mbus::CntReqType::CREATE_OBJECT);
	req.set_parent_id(1);
	req.add_properties(std::move(cls_prop));

	frigg::String<KernelAlloc> ser(*kernelAlloc);
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

void handleBind(LaneHandle object_lane, FbInfo *fbinfo) {
	auto branch = fiberAccept(object_lane);
	assert(branch);

	auto buffer = fiberRecv(branch);
	managarm::mbus::SvrRequest<KernelAlloc> req(*kernelAlloc);
	req.ParseFromArray(buffer.data(), buffer.size());
	assert(req.req_type() == managarm::mbus::SvrReqType::BIND);
	
	managarm::mbus::CntResponse<KernelAlloc> resp(*kernelAlloc);
	resp.set_error(managarm::mbus::Error::SUCCESS);

	frigg::String<KernelAlloc> ser(*kernelAlloc);
	resp.SerializeToString(&ser);
	fiberSend(branch, ser.data(), ser.size());

	auto stream = createStream();
	fiberPushDescriptor(branch, LaneDescriptor{stream.get<1>()});

	// TODO: Do this in an own fiber.
	KernelFiber::run([lane = stream.get<0>(), info = fbinfo] () {
		while(true) {
			if(!handleReq(lane, info))
				break;
		}
	});
}

} // anonymous namespace

void initializeFb(uint64_t address, uint64_t pitch, uint64_t width,
		uint64_t height, uint64_t bpp, uint64_t type) {
	auto fb_info = frigg::construct<FbInfo>(*kernelAlloc);
	fb_info->address = address;
	fb_info->pitch = pitch;
	fb_info->width = width;
	fb_info->height = height;
	fb_info->bpp = bpp;
	fb_info->type = type;
	
	assert(!(address & (kPageSize - 1)));
	fb_info->memory = frigg::makeShared<HardwareMemory>(*kernelAlloc,
			address & ~(kPageSize - 1),
			(height * pitch + (kPageSize - 1)) & ~(kPageSize - 1));	

	// Create a fiber to manage requests to the FB mbus object.
	KernelFiber::run([=] {
		auto object_lane = createObject(*mbusClient);
		while(true)
			handleBind(object_lane, fb_info);
	});
}

} // namespace thor

