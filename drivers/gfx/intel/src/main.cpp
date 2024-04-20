
#include <assert.h>
#include <string.h>
#include <iostream>
#include <memory>

#include <arch/mem_space.hpp>
#include <async/result.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>

#include "spec.hpp"
#include "intel.hpp"

struct [[ gnu::packed ]] DisplayData {
	uint8_t magic[8];
	uint16_t vendorId;
	uint16_t productId;
	uint32_t serialNumber;
	uint8_t manufactureWeek;
	uint8_t manufactureYear;
	uint8_t structVersion;
	uint8_t structRevision;
	uint8_t inputParameters;
	uint8_t screenWidth;
	uint8_t screenHeight;
	uint8_t gamma;
	uint8_t features;
	uint8_t colorCoordinates[10];
	uint8_t estTimings1;
	uint8_t estTimings2;
	uint8_t vendorTimings;
	struct {
		uint8_t resolution;
		uint8_t frequency;
	} standardTimings[8];
	struct {
		uint16_t pixelClock;
		uint8_t horzActive;
		uint8_t horzBlank;
		uint8_t horzActiveBlankMsb;
		uint8_t vertActive;
		uint8_t vertBlank;
		uint8_t vertActiveBlankMsb;
		uint8_t horzSyncOffset;
		uint8_t horzSyncPulse;
		uint8_t vertSync;
		uint8_t syncMsb;
		uint8_t dimensionWidth;
		uint8_t dimensionHeight;
		uint8_t dimensionMsb;
		uint8_t horzBorder;
		uint8_t vertBorder;
		uint8_t features;
	} detailTimings[4];
	uint8_t numExtensions;
	uint8_t checksum;
};
static_assert(sizeof(DisplayData) == 128, "Bad sizeof(DisplayData)");

Mode edidToMode(DisplayData edid) {
	Mode mode;

	assert(edid.detailTimings[0].pixelClock);
	mode.dot = edid.detailTimings[0].pixelClock * 10;

	// For now we do not support borders.
	assert(!edid.detailTimings[0].horzBorder);
	assert(!edid.detailTimings[0].vertBorder);

	auto horz_active = edid.detailTimings[0].horzActive
			| (static_cast<unsigned int>(edid.detailTimings[0].horzActiveBlankMsb >> 4) << 8);
	auto horz_blank = edid.detailTimings[0].horzBlank
			| (static_cast<unsigned int>(edid.detailTimings[0].horzActiveBlankMsb & 0xF) << 8);
	auto horz_sync_offset = edid.detailTimings[0].horzSyncOffset
			| (static_cast<unsigned int>(edid.detailTimings[0].syncMsb >> 6) << 8);
	auto horz_sync_pulse = edid.detailTimings[0].horzSyncPulse
			| ((static_cast<unsigned int>(edid.detailTimings[0].syncMsb >> 4) & 0x3) << 8);

	std::cout << "horizontal: " << horz_active << ", " << horz_blank
			<< ", " << horz_sync_offset << ", " << horz_sync_pulse << std::endl;
	mode.horizontal.active = horz_active;
	mode.horizontal.syncStart = horz_active + horz_sync_offset;
	mode.horizontal.syncEnd = horz_active + horz_sync_offset + horz_sync_pulse;
	mode.horizontal.total = horz_active + horz_blank;
	mode.horizontal.dump();

	auto vert_active =  edid.detailTimings[0].vertActive
			| (static_cast<unsigned int>(edid.detailTimings[0].vertActiveBlankMsb >> 4) << 8);
	auto vert_blank = edid.detailTimings[0].vertBlank
			| (static_cast<unsigned int>(edid.detailTimings[0].vertActiveBlankMsb & 0xF) << 8);
	auto vert_sync_offset = (edid.detailTimings[0].vertSync >> 4)
			| ((static_cast<unsigned int>(edid.detailTimings[0].syncMsb >> 2) & 0x3) << 4);
	auto vert_sync_pulse = (edid.detailTimings[0].vertSync & 0xF)
			| (static_cast<unsigned int>(edid.detailTimings[0].syncMsb & 0x3) << 4);
	
	std::cout << "vertical: " << vert_active << ", " << vert_blank
			<< ", " << vert_sync_offset << ", " << vert_sync_pulse << std::endl;
	mode.vertical.active = vert_active;
	mode.vertical.syncStart = vert_active + vert_sync_offset;
	mode.vertical.syncEnd = vert_active + vert_sync_offset + vert_sync_pulse;
	mode.vertical.total = vert_active + vert_blank;
	mode.vertical.dump();

	return mode;
}

bool checkParams(PllParams params, int refclock, PllLimits limits) {
	if(params.n < limits.n.min || params.n > limits.n.max)
		return false;
	if(params.m1 < limits.m1.min || params.m1 > limits.m1.max)
		return false;
	if(params.m2 < limits.m2.min || params.m2 > limits.m2.max)
		return false;
	if(params.p1 < limits.p1.min || params.p1 > limits.p1.max)
		return false;
	
	if(params.m1 <= params.m2)
		return false;

	auto m = params.computeM();
	auto p = params.computeP();

	if(m < limits.m.min || m > limits.m.max)
		return false;
	if(p < limits.p.min || p > limits.p.max)
		return false;

	auto dot = params.computeDot(refclock);
	auto vco = params.computeVco(refclock);

	if(dot < limits.dot.min || dot > limits.dot.max)
		return false;
	if(vco < limits.vco.min || dot > limits.vco.max)
		return false;
	
	return true;
}

PllParams findParams(int target, int refclock, PllLimits limits) {
	PllParams params;

	// TODO: This is fine for G4x.
	params.p2 = 10;

	for(params.n = limits.n.min; params.n <= limits.n.max; ++params.n) {
		for(params.m1 = limits.m1.max; params.m1 >= limits.m1.min; --params.m1) {
			for(params.m2 = limits.m2.max; params.m2 >= limits.m2.min; --params.m2) {
				for(params.p1 = limits.p1.max; params.p1 >= limits.p1.min; --params.p1) {
					if(!checkParams(params, refclock, limits))
						continue;

					if(params.computeDot(refclock) == target)
						return params;
				}
			}
		}
	}

	throw std::runtime_error("No DPLL parameters for target dot clock");
}

int computeSdvoMultiplier(int pixel_clock) {
	if(pixel_clock >= 100000) {
		return 1;
	}else if(pixel_clock >= 50000) {
		return 2;
	}else{
		assert(pixel_clock >= 25000);
		return 4;
	}
}

struct Controller {
	Controller(arch::mem_space ctrl, void *memory)
	: _ctrl{ctrl}, _memory{memory} { }

	void run();

private:
	// ------------------------------------------------------------------------
	// GMBUS functions.
	// ------------------------------------------------------------------------
	void i2cWrite(unsigned int address, const void *buffer, size_t size);
	void i2cRead(unsigned int address, void *buffer, size_t size);

	void _waitForGmbusProgress();
	void _waitForGmbusCompletion();

	// ------------------------------------------------------------------------
	// DPLL programming functions.
	// ------------------------------------------------------------------------

	void disableDpll();
	void programDpll(PllParams params, int multiplier);
	void dumpDpll();

	// ------------------------------------------------------------------------
	// Pipe programming functions.
	// ------------------------------------------------------------------------

	void disablePipe();
	void programPipe(Mode mode);
	void dumpPipe();
	
	// ------------------------------------------------------------------------
	// Plane handling functions.
	// ------------------------------------------------------------------------

	void disablePlane();
	void enablePlane(Framebuffer *fb);
	
	// ------------------------------------------------------------------------
	// Port handling functions.
	// ------------------------------------------------------------------------

	void disableDac();
	void enableDac();

	// ------------------------------------------------------------------------
	// Miscellaneous functions.
	// ------------------------------------------------------------------------

	void relinquishVga();

private:
	arch::mem_space _ctrl;
	void *_memory;
};

void Controller::run() {
	uint8_t offset = 0;
	DisplayData edid;

	_ctrl.store(regs::gmbusSelect, gmbus_select::pairSelect(PinPair::analog));
	i2cWrite(0x50, &offset, 1);
	i2cRead(0x50, &edid, 128);

	auto mode = edidToMode(edid);

	// Set up a nice framebuffer for our mode.
	Framebuffer fb;
	fb.width = mode.horizontal.active;
	fb.height = mode.vertical.active;
	fb.stride = fb.width * 4;
	fb.address = 0;

	auto plane = reinterpret_cast<uint32_t *>(_memory);
	for(size_t x = 0; x < fb.width; x++)
		for(size_t y = 0; y < fb.height; y++)
			plane[y * fb.width + x] = (x / 5) | ((y / 4) << 8);

	// Perform the mode setting.
	auto multiplier = computeSdvoMultiplier(mode.dot);
	auto params = findParams(mode.dot * multiplier, 96000, limitsG45);
	
	disableDac();
	disablePipe();
	disableDpll();
	relinquishVga();

	programDpll(params, multiplier);
	dumpDpll();

	programPipe(mode);
	dumpPipe();
	enablePlane(&fb);
	enableDac();
}

// ------------------------------------------------------------------------
// GMBUS functions.
// ------------------------------------------------------------------------

void Controller::i2cWrite(unsigned int address, const void *buffer, size_t size) {
	size_t progress = 0;
	auto view = reinterpret_cast<const unsigned char *>(buffer);
	auto stream = [&] {
		uint32_t data = 0;
		for(size_t i = 0; i < 4; ++i) {
			if(progress == size)
				break;
			data |= uint32_t{view[progress++]} << (8 * i);
		}
		_ctrl.store(regs::gmbusData, data);
	};

	// Asymmetry to i2cRead(): We fill the data buffer before issuing the cycle.
	stream();
	_ctrl.store(regs::gmbusCommand, gmbus_command::address(address)
			| gmbus_command::byteCount(size) | gmbus_command::cycleSelect(BusCycle::wait)
			| gmbus_command::softwareReady(true));
//	std::cout << "gfx_intel i2c: Wait" << std::endl;
	_waitForGmbusProgress();
//	std::cout << "gfx_intel i2c: OK" << std::endl;

	while(progress < size) {
		stream();
		_waitForGmbusProgress();
	}
	
	_waitForGmbusCompletion();
}

void Controller::i2cRead(unsigned int address, void *buffer, size_t size) {
	size_t progress = 0;
	auto view = reinterpret_cast<unsigned char *>(buffer);
	auto stream = [&] {
		uint32_t data = _ctrl.load(regs::gmbusData);
		for(size_t i = 0; i < 4; ++i) {
			if(progress == size)
				break;
			view[progress++] = data >> (8 * i);
		}
	};

	_ctrl.store(regs::gmbusCommand, gmbus_command::issueRead(true)
			| gmbus_command::address(address)
			| gmbus_command::byteCount(size) | gmbus_command::cycleSelect(BusCycle::wait)
			| gmbus_command::softwareReady(true));

	while(progress < size) {
//		std::cout << "gfx_intel i2c: Wait" << std::endl;
		_waitForGmbusProgress();
//		std::cout << "gfx_intel i2c: Done" << std::endl;
		stream();
	}

	_waitForGmbusCompletion();
}

void Controller::_waitForGmbusProgress() {
	while(true) {
		auto status = _ctrl.load(regs::gmbusStatus);
		assert(!(status & gmbus_status::nakIndicator));
		if(status & gmbus_status::hardwareReady)
			break;
	}	
}

void Controller::_waitForGmbusCompletion() {
	while(true) {
		auto status = _ctrl.load(regs::gmbusStatus);
		assert(!(status & gmbus_status::nakIndicator));
		if(status & gmbus_status::waitPhase)
			break;
	}	
}

// ------------------------------------------------------------------------
// DPLL programming functions.
// ------------------------------------------------------------------------

void Controller::disableDpll() {
	auto bits = _ctrl.load(regs::pllControl);
	assert(bits & pll_control::enablePll);
	_ctrl.store(regs::pllControl, bits & ~pll_control::enablePll);
}

void Controller::programDpll(PllParams params, int multiplier) {
	_ctrl.store(regs::pllDivisor1, pll_divisor::m2(params.m2)
			| pll_divisor::m1(params.m1) | pll_divisor::n(params.n));
	_ctrl.store(regs::pllDivisor2, pll_divisor::m2(params.m2)
			| pll_divisor::m1(params.m1) | pll_divisor::n(params.n));
	
	_ctrl.store(regs::pllControl, pll_control::enablePll(false));

	_ctrl.store(regs::pllControl, pll_control::phase(6)
			| pll_control::encodedP1(1 << (params.p1 - 1))
			| pll_control::modeSelect(1) | pll_control::disableVga(true)
			| pll_control::enablePll(true));
	_ctrl.load(regs::pllControl);

	uint64_t ticks, now;
	HEL_CHECK(helGetClock(&ticks));
	do {
		HEL_CHECK(helGetClock(&now));
	} while(now - ticks <= 150000);
	
	std::cout << "State: " << (_ctrl.load(regs::pllControl) & pll_control::enablePll)
			<< std::endl;

	_ctrl.store(regs::busMultiplier, bus_multiplier::vgaMultiplier(multiplier - 1)
			| bus_multiplier::dacMultiplier(multiplier - 1));
	
	for(int i = 0; i < 3; i++) {
		_ctrl.store(regs::pllControl, pll_control::phase(6)
				| pll_control::encodedP1(1 << (params.p1 - 1))
				| pll_control::modeSelect(1) | pll_control::disableVga(true)
				| pll_control::enablePll(true));
		_ctrl.load(regs::pllControl);

		uint64_t ticks, now;
		HEL_CHECK(helGetClock(&ticks));
		do {
			HEL_CHECK(helGetClock(&now));
		} while(now - ticks <= 150000);
	
		std::cout << "State: " << (_ctrl.load(regs::pllControl) & pll_control::enablePll)
				<< std::endl;
	}
}

void Controller::dumpDpll() {
	auto control = _ctrl.load(regs::pllControl);
	auto divisor1 = _ctrl.load(regs::pllDivisor1);

	if(control & pll_control::enablePll) {
		std::cout << "gfx_intel: DPLL is running." << std::endl;
	}else{
		std::cout << "gfx_intel: DPLL is disabled." << std::endl;
	}

	PllParams params;
	params.n = divisor1 & pll_divisor::n;
	params.m1 = divisor1 & pll_divisor::m1;
	params.m2 = divisor1 & pll_divisor::m2;
	params.p1 = __builtin_ffs(control & pll_control::encodedP1);
	params.p2 = 10; // TODO: Actually read this.
	params.dump(96000);
}

// ------------------------------------------------------------------------
// Pipe programming functions.
// ------------------------------------------------------------------------

void Controller::disablePipe() {
	auto bits = _ctrl.load(regs::pipeConfig);
	std::cout << "Pipe config: " << static_cast<uint32_t>(bits) << std::endl;
	assert(bits & pipe_config::enablePipe);
	assert(bits & pipe_config::pipeStatus);
	_ctrl.store(regs::pipeConfig, bits & ~pipe_config::enablePipe);
	
	std::cout << "After disable: " << (_ctrl.load(regs::pipeConfig)
			& pipe_config::pipeStatus) << std::endl;
	while(_ctrl.load(regs::pipeConfig) & pipe_config::pipeStatus) {
		// Busy wait until the pipe is shut off.
	}

	std::cout << "Pipe disabled" << std::endl;
}

void Controller::programPipe(Mode mode) {
	// Program the display timings.
	_ctrl.store(regs::htotal, hvtotal::active(mode.horizontal.active - 1)
			| hvtotal::total(mode.horizontal.total - 1));
	_ctrl.store(regs::hblank, hvblank::start(mode.horizontal.blankingStart() - 1)
			| hvblank::end(mode.horizontal.blankingEnd() - 1));
	_ctrl.store(regs::hsync, hvsync::start(mode.horizontal.syncStart - 1)
			| hvsync::end(mode.horizontal.syncEnd - 1));
	
	_ctrl.store(regs::vtotal, hvtotal::active(mode.vertical.active - 1)
			| hvtotal::total(mode.vertical.total - 1));
	_ctrl.store(regs::vblank, hvblank::start(mode.vertical.blankingStart() - 1)
			| hvblank::end(mode.vertical.blankingEnd() - 1));
	_ctrl.store(regs::vsync, hvsync::start(mode.vertical.syncStart - 1)
			| hvsync::end(mode.vertical.syncEnd - 1));
	
	_ctrl.store(regs::sourceSize, source_size::vertical(mode.vertical.active - 1)
			| source_size::horizontal(mode.horizontal.active - 1));
	
	// Enable the pipe.
	auto bits = _ctrl.load(regs::pipeConfig);
	assert(!(bits & pipe_config::enablePipe));
	assert(!(bits & pipe_config::pipeStatus));
	_ctrl.store(regs::pipeConfig, bits | pipe_config::enablePipe(true));
	
	while(!(_ctrl.load(regs::pipeConfig) & pipe_config::pipeStatus)) {
		// Busy wait until the pipe is ready.
	}

	std::cout << "Pipe enabled" << std::endl;
}

void Controller::dumpPipe() {
	auto htotal = _ctrl.load(regs::htotal);
	auto hblank = _ctrl.load(regs::hblank);
	auto hsync = _ctrl.load(regs::hsync);
	auto vtotal = _ctrl.load(regs::vtotal);
	auto vblank = _ctrl.load(regs::vblank);
	auto vsync = _ctrl.load(regs::vsync);

	Timings horizontal;
	Timings vertical;
	horizontal.active = (htotal & hvtotal::active) + 1;
	horizontal.syncStart = (hsync & hvsync::start) + 1;
	horizontal.syncEnd = (hsync & hvsync::end) + 1;
	horizontal.total = (htotal & hvtotal::total) + 1;
	vertical.active = (vtotal & hvtotal::active) + 1;
	vertical.syncStart = (vsync & hvsync::start) + 1;
	vertical.syncEnd = (vsync & hvsync::end) + 1;
	vertical.total = (vtotal & hvtotal::total) + 1;

	horizontal.dump();
	std::cout << ((hblank & hvblank::start) + 1)
			<< ", " << ((hblank & hvblank::end) + 1) << std::endl;
	vertical.dump();
	std::cout << ((vblank & hvblank::start) + 1)
			<< ", " << ((vblank & hvblank::end) + 1) << std::endl;
}

// ------------------------------------------------------------------------
// Plane handling functions.
// ------------------------------------------------------------------------

void Controller::disablePlane() {
	auto bits = _ctrl.load(regs::planeControl);
	assert(bits & plane_control::enablePlane);
	_ctrl.store(regs::planeControl, bits & ~plane_control::enablePlane);
}

void Controller::enablePlane(Framebuffer *fb) {
	assert(!(fb->stride % 64));
	_ctrl.store(regs::planeOffset, 0);
	_ctrl.store(regs::planeStride, fb->stride);
	_ctrl.store(regs::planeAddress, fb->address);

	auto bits = _ctrl.load(regs::planeControl);
	std::cout << "Plane control: " << static_cast<uint32_t>(bits) << std::endl;
	assert(!(bits & plane_control::enablePlane));
	_ctrl.store(regs::planeControl, (bits & ~plane_control::pixelFormat)
			| plane_control::pixelFormat(PrimaryFormat::RGBX8888)
			| plane_control::enablePlane(true));
}

// ------------------------------------------------------------------------
// Port handling functions.
// ------------------------------------------------------------------------

void Controller::disableDac() {
	auto bits = _ctrl.load(regs::dacPort);
	std::cout << "DAC Port: " << static_cast<uint32_t>(bits) << std::endl;
	assert(bits & dac_port::enableDac);
	_ctrl.store(regs::dacPort, bits & ~dac_port::enableDac);
}

void Controller::enableDac() {
	auto bits = _ctrl.load(regs::dacPort);
	assert(!(bits & dac_port::enableDac));
	_ctrl.store(regs::dacPort, bits | dac_port::enableDac(true));
}

// ------------------------------------------------------------------------
// Miscellanious functions.
// ------------------------------------------------------------------------

void Controller::relinquishVga() {
	auto bits = _ctrl.load(regs::vgaControl);
	assert(!(bits & vga_control::disableVga));
	_ctrl.store(regs::vgaControl, (bits & ~vga_control::centeringMode)
			| vga_control::disableVga(true));
}

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

async::detached bindController(mbus_ng::Entity hwEntity) {
	protocols::hw::Device device((co_await hwEntity.getRemoteLane()).unwrap());
	auto info = co_await device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	assert(info.barInfo[2].ioType == protocols::hw::IoType::kIoTypeMemory);
	assert(!info.barInfo[0].offset);
	assert(!info.barInfo[2].offset);
	auto ctrl_bar = co_await device.accessBar(0);
	auto memory_bar = co_await device.accessBar(2);
//	auto irq = co_await device.accessIrq();
	
	void *ctrl_window, *memory_window;
	HEL_CHECK(helMapMemory(ctrl_bar.getHandle(), kHelNullHandle, nullptr,
			0, 0x8'0000, kHelMapProtRead | kHelMapProtWrite,
			&ctrl_window));
	HEL_CHECK(helMapMemory(memory_bar.getHandle(), kHelNullHandle, nullptr,
			0, 0x1000'0000, kHelMapProtRead | kHelMapProtWrite,
			&memory_window));

	Controller controller{arch::mem_space(ctrl_window), memory_window};
	controller.run();
}

async::detached observeControllers() {
	auto filter = mbus_ng::Conjunction{{
		mbus_ng::EqualsFilter{"pci-vendor", "8086"},
		mbus_ng::EqualsFilter{"pci-device", "2e32"}
	}};

	auto enumerator = mbus_ng::Instance::global().enumerate(filter);
	while (true) {
		auto [_, events] = (co_await enumerator.nextEvents()).unwrap();

		for (auto &event : events) {
			if (event.type != mbus_ng::EnumerationEvent::Type::created)
				continue;

			auto entity = co_await mbus_ng::Instance::global().getEntity(event.id);
			std::cout << "gfx_intel: Detected controller" << std::endl;
			bindController(std::move(entity));
		}
	}
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting Intel graphics driver\n");

	observeControllers();
	async::run_forever(helix::currentDispatcher);
}
 
