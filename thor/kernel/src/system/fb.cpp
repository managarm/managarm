
#include <frigg/debug.hpp>
#include <arch/io_space.hpp>
#include "../arch/x86/cpu.hpp"
#include "../arch/x86/hpet.hpp"
#include "../generic/fiber.hpp"
#include "../generic/io.hpp"
#include "../generic/kernel_heap.hpp"
#include "../generic/service_helpers.hpp"
#include "pci/pci.hpp"

#include "fb.hpp"
#include "boot-screen.hpp"

#include <mbus.frigg_pb.hpp>
#include <hw.frigg_pb.hpp>

extern uint8_t fontBitmap[];

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
constexpr uint32_t defaultBg = rgb(16, 16, 16);

struct FbDisplay : TextDisplay {
	FbDisplay(void *window, unsigned int width, unsigned int height, size_t pitch)
	: _window{reinterpret_cast<uint32_t *>(window)},
			_width{width}, _height{height}, _pitch{pitch / sizeof(uint32_t)} {
		assert(!(pitch % sizeof(uint32_t)));
		_clearScreen(defaultBg);
	}

	int getWidth() override;
	int getHeight() override;
	
	void setChars(unsigned int x, unsigned int y,
			const char *c, int count, int fg, int bg) override;
	void setBlanks(unsigned int x, unsigned int y, int count, int bg) override;

private:
	void _clearScreen(uint32_t rgb_color);

	volatile uint32_t *_window;
	unsigned int _width;
	unsigned int _height;
	size_t _pitch;
};

int FbDisplay::getWidth() {
	return _width / fontWidth;
}

int FbDisplay::getHeight() {
	return _height / fontHeight;
}

void FbDisplay::setChars(unsigned int x, unsigned int y,
		const char *c, int count, int fg, int bg) {
	auto fg_rgb = rgbColor[fg];
	auto bg_rgb = (bg < 0) ? defaultBg : rgbColor[bg]; 

	auto dest_line = _window + y * fontHeight * _pitch + x * fontWidth;
	for(size_t i = 0; i < fontHeight; i++) {
		auto dest = dest_line;
		for(int k = 0; k < count; k++) {
			auto dc = (c[k] >= 32 && c[k] <= 127) ? c[k] : 127;
			auto fontbits = fontBitmap[(dc - 32) * fontHeight + i];
			for(size_t j = 0; j < fontWidth; j++) {
				int bit = (1 << ((fontWidth - 1) - j));
				*dest++ = (fontbits & bit) ? fg_rgb : bg_rgb;
			}
		}
		dest_line += _pitch;
	}
}

void FbDisplay::setBlanks(unsigned int x, unsigned int y, int count, int bg) {
	auto bg_rgb = (bg < 0) ? defaultBg : rgbColor[bg]; 

	auto dest_line = _window + y * fontHeight * _pitch + x * fontWidth;
	for(size_t i = 0; i < fontHeight; i++) {
		auto dest = dest_line;
		for(int k = 0; k < count; k++) {
			for(size_t j = 0; j < fontWidth; j++)
				*dest++ = bg_rgb;
		}
		dest_line += _pitch;
	}
}

void FbDisplay::_clearScreen(uint32_t rgb_color) {
	auto dest_line = _window;
	for(size_t i = 0; i < _height; i++) {
		auto dest = dest_line;
		for(size_t j = 0; j < _width; j++)
			*dest++ = rgb_color;
		dest_line += _pitch;
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
			fb_info->width, fb_info->height, fb_info->pitch);
	auto screen = frigg::construct<BootScreen>(*kernelAlloc, display);

	//enableLogHandler(screen);

	// Try to attached the framebuffer to a PCI device.
	pci::PciDevice *owner = nullptr;
	for(auto it = pci::allDevices->begin(); it != pci::allDevices->end(); ++it) {
		auto checkBars = [&] () -> bool {
			for(int i = 0; i < 6; i++) {
				if((*it)->bars[i].type != pci::PciDevice::kBarMemory)
					continue;
				// TODO: Careful about overflow here.
				if(address >= (*it)->bars[i].address && address + height * pitch
						<= (*it)->bars[i].address + (*it)->bars[i].length)
					return true;
			}
			
			return false;
		};

		if(checkBars()) {
			assert(!owner);
			owner = *it;
		}
	}

	if(!owner)
		frigg::panicLogger() << "thor: Could not find owner for boot framebuffer" << frigg::endLog;
	frigg::infoLogger() << "thor: Boot framebuffer is attached to PCI device "
			<< owner->bus << "." << owner->slot << "." << owner->function << frigg::endLog;
	owner->associatedScreen = screen;

	// Create a fiber to manage requests to the FB mbus object.
	KernelFiber::run([=] {
		auto object_lane = createObject(*mbusClient);
		while(true)
			handleBind(object_lane, fb_info);
	});
}

} // namespace thor

