
#include <frigg/debug.hpp>
#include <arch/io_space.hpp>
#include "../arch/x86/cpu.hpp"
#include "../arch/x86/hpet.hpp"
#include "../generic/fiber.hpp"
#include "../generic/io.hpp"
#include "../generic/kernel_heap.hpp"
#include "../generic/service_helpers.hpp"

#include "fb.hpp"
#include "boot-screen.hpp"

#include <mbus.frigg_pb.hpp>
#include <hw.frigg_pb.hpp>

extern unsigned char vga_font[];

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
	
	void *window;
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
		MemoryBundleDescriptor descriptor{info->memory};
		
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

// ------------------------------------------------------------------------
// window handling
// ------------------------------------------------------------------------

constexpr size_t fontHeight = 16;
constexpr size_t fontWidth = 8;

constexpr uint32_t rgb(int r, int g, int b) {
	return (r << 16) | (g << 8) | b;
}

constexpr uint32_t rgbColor[16] = {
	rgb(1, 1, 1),
	rgb(222, 56, 43),
	rgb(57, 181, 74),
	rgb(255, 199, 6),
	rgb(0, 111, 184),
	rgb(118, 38, 113),
	rgb(44, 181, 233),
	rgb(204, 204, 204),
	rgb(128, 128, 128),
	rgb(255, 0, 0),
	rgb(0, 255, 0),
	rgb(255, 255, 0),
	rgb(0, 0, 255),
	rgb(255, 0, 255),
	rgb(0, 255, 255),
	rgb(255, 255, 255) 
};
constexpr uint32_t defaultBg = rgb(25, 25, 112);

struct FbDisplay : TextDisplay {
	FbDisplay(void *window, unsigned int width, uint64_t pitch, unsigned int height)
	:_window(window), _width(width), _pitch(pitch), _height(height) {
		clearScreen(defaultBg);
	}
	
	void setChar(unsigned int x, unsigned int y, char c, int fg, int bg) override;
	int getWidth() override;
	int getHeight() override;

private:
	void setPixel(unsigned int x, unsigned int y, uint32_t color);
	void clearScreen(uint32_t color);

	void *_window;
	unsigned int _width;
	uint64_t _pitch;
	unsigned int _height;
};

void FbDisplay::setChar(unsigned int x, unsigned int y, char c,
		int fg, int bg) {
	unsigned int val;
	for(size_t i = 0; i < fontHeight; i++) {
		val = vga_font[(c - 32) * fontHeight + i];
		for(size_t j = 0; j < fontWidth; j++) {
			if(val & (1 << ((fontWidth - 1) - j))) {
				setPixel(x * fontWidth + j, y * fontHeight + i, rgbColor[fg]);
			}else {
				setPixel(x * fontWidth + j, y * fontHeight + i, (bg < 0) ? defaultBg : rgbColor[bg]); 
			}
		}
	}
}

int FbDisplay::getWidth() {
	return _width / fontWidth;
}

int FbDisplay::getHeight() {
	return _height / fontHeight;
}

void FbDisplay::setPixel(unsigned int x, unsigned int y, uint32_t color) {
	memcpy(reinterpret_cast<char *>(_window)
			+ y * _pitch + x * sizeof(uint32_t), &color, sizeof(uint32_t));
}

void FbDisplay::clearScreen(uint32_t color) {
	for(size_t y = 0; y < _height; y++) {
		for(size_t x = 0; x < _width; x++) {
			setPixel(x, y, color);
		}
	}
}

void initializeFb(uint64_t address, uint64_t pitch, uint64_t width,
		uint64_t height, uint64_t bpp, uint64_t type) {
	auto fb_info = frigg::construct<FbInfo>(*kernelAlloc);
	fb_info->address = address;
	fb_info->pitch = pitch;
	fb_info->width = width;
	fb_info->height = height;
	fb_info->bpp = bpp;
	fb_info->type = type;

	auto window_size = (height * pitch + (kPageSize - 1)) & ~(kPageSize - 1);
	assert(window_size <= 0x1'000'000);
	fb_info->window = KernelVirtualMemory::global().allocate(0x1'000'000);
	for(size_t pg = 0; pg < window_size; pg += kPageSize)
		KernelPageSpace::global().mapSingle4k(VirtualAddr(fb_info->window) + pg,
				address + pg, page_access::write);
	
	assert(!(address & (kPageSize - 1)));
	fb_info->memory = frigg::makeShared<HardwareMemory>(*kernelAlloc,
			address & ~(kPageSize - 1),
			(height * pitch + (kPageSize - 1)) & ~(kPageSize - 1));	

	auto display = frigg::construct<FbDisplay>(*kernelAlloc, fb_info->window,
			fb_info->width, fb_info->pitch, fb_info->height);
	auto bootScreen = frigg::construct<BootScreen>(*kernelAlloc, display);

	enableLogHandler(bootScreen);

	// Create a fiber to manage requests to the FB mbus object.
	KernelFiber::run([=] {
		auto object_lane = createObject(*mbusClient);
		while(true)
			handleBind(object_lane, fb_info);
	});
}

} // namespace thor

