
#include <render-text.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/fiber.hpp>
#include <thor-internal/io.hpp>
#include <thor-internal/kernel_heap.hpp>
#include <thor-internal/pci/pci.hpp>

#include <thor-internal/framebuffer/fb.hpp>
#include <thor-internal/framebuffer/boot-screen.hpp>

#include <hw.frigg_bragi.hpp>

namespace thor {

// ------------------------------------------------------------------------
// window handling
// ------------------------------------------------------------------------

constexpr size_t fontHeight = 16;
constexpr size_t fontWidth = 8;

struct FbDisplay final : TextDisplay {
	FbDisplay(void *ptr, unsigned int width, unsigned int height, size_t pitch)
	: _width{width}, _height{height}, _pitch{pitch / sizeof(uint32_t)} {
		assert(!(pitch % sizeof(uint32_t)));
		setWindow(ptr);
		_clearScreen(defaultBg);
	}

	void setWindow(void *ptr) {
		_window = reinterpret_cast<uint32_t *>(ptr);
	}

	size_t getWidth() override;
	size_t getHeight() override;

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

size_t FbDisplay::getWidth() {
	return _width / fontWidth;
}

size_t FbDisplay::getHeight() {
	return _height / fontHeight;
}

void FbDisplay::setChars(unsigned int x, unsigned int y,
		const char *c, int count, int fg, int bg) {
	renderChars((void *)_window, _pitch, x, y, c, count, fg, bg,
			std::integral_constant<int, fontWidth>{},
			std::integral_constant<int, fontHeight>{});
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

namespace {
	frg::manual_box<FbInfo> bootInfo;
	frg::manual_box<FbDisplay> bootDisplay;
	frg::manual_box<BootScreen> bootScreen;
}

void initializeBootFb(uint64_t address, uint64_t pitch, uint64_t width,
		uint64_t height, uint64_t bpp, uint64_t type, void *early_window) {
	bootInfo.initialize();
	auto fb_info = bootInfo.get();
	fb_info->address = address;
	fb_info->pitch = pitch;
	fb_info->width = width;
	fb_info->height = height;
	fb_info->bpp = bpp;
	fb_info->type = type;

	// Initialize the framebuffer with a lower-half window.
	bootDisplay.initialize(early_window,
			fb_info->width, fb_info->height, fb_info->pitch);
	bootScreen.initialize(bootDisplay.get());

	enableLogHandler(bootScreen.get());
}

void transitionBootFb() {
	if(!bootInfo->address) {
		infoLogger() << "thor: No boot framebuffer" << frg::endlog;
		return;
	}

	auto windowSize = (bootInfo->height * bootInfo->pitch + (kPageSize - 1)) & ~(kPageSize - 1);
	auto window = KernelVirtualMemory::global().allocate(windowSize);
	for(size_t pg = 0; pg < windowSize; pg += kPageSize)
		KernelPageSpace::global().mapSingle4k(VirtualAddr(window) + pg,
				bootInfo->address + pg, page_access::write, CachingMode::writeCombine);

	// Transition to the kernel mapping window.
	bootDisplay->setWindow(window);

	assert(!(bootInfo->address & (kPageSize - 1)));
	bootInfo->memory = smarter::allocate_shared<HardwareMemory>(*kernelAlloc,
			bootInfo->address & ~(kPageSize - 1),
			(bootInfo->height * bootInfo->pitch + (kPageSize - 1)) & ~(kPageSize - 1),
			CachingMode::writeCombine);

	// Try to attached the framebuffer to a PCI device.
	pci::PciDevice *owner = nullptr;
	for (auto dev : *pci::allDevices) {
		auto checkBars = [&] () -> bool {
			for(int i = 0; i < 6; i++) {
				if(dev->bars[i].type != pci::PciBar::kBarMemory)
					continue;
				// TODO: Careful about overflow here.
				auto bar_begin = dev->bars[i].address;
				auto bar_end = dev->bars[i].address + dev->bars[i].length;
				if(bootInfo->address >= bar_begin
						&& bootInfo->address + bootInfo->height * bootInfo->pitch <= bar_end)
					return true;
			}

			return false;
		};

		if(checkBars()) {
			assert(!owner);
			owner = dev.get();
		}
	}

	if(!owner) {
		infoLogger() << "thor: Could not find owner for boot framebuffer" << frg::endlog;
		return;
	}

	infoLogger() << "thor: Boot framebuffer is attached to PCI device "
			<< owner->bus << "." << owner->slot << "." << owner->function << frg::endlog;
	owner->associatedFrameBuffer = bootInfo.get();
	owner->associatedScreen = bootScreen.get();
}

} // namespace thor

