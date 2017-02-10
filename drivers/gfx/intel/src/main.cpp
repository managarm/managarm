
#include <assert.h>
#include <string.h>
#include <iostream>
#include <memory>

#include <arch/mem_space.hpp>
#include <async/result.hpp>
#include <cofiber.hpp>
#include <protocols/hw/client.hpp>
#include <protocols/mbus/client.hpp>

#include "spec.hpp"
#include "intel.hpp"

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

struct Controller {
	Controller(arch::mem_space ctrl, void *memory)
	: _ctrl{ctrl}, _memory{memory} { }

	void run();

private:
	// ------------------------------------------------------------------------
	// DPLL programming functions.
	// ------------------------------------------------------------------------

	void disableDpll();
	void programDpll(PllParams params);
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
	void enablePlane();
	
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
	Mode mode{
		25175,
		{ 640, 656, 752, 800 },
		{ 480, 490, 492, 525 }
	};
	auto params = findParams(100800, 96000, limitsG45);
	
	disableDac();
	disablePipe();
	disableDpll();
	relinquishVga();

	programDpll(params);
	dumpDpll();

	programPipe(mode);
	dumpPipe();
	enablePlane();
	enableDac();

	auto plane = reinterpret_cast<uint32_t *>(_memory);
	for(size_t x = 0; x < 640; x++)
		for(size_t y = 0; y < 480; y++)
			plane[y * 640 + x] = (x / 3) | ((y / 2) << 8);
}

// ------------------------------------------------------------------------
// DPLL programming functions.
// ------------------------------------------------------------------------

void Controller::disableDpll() {
	auto bits = _ctrl.load(regs::pllControl);
	assert(bits & pll_control::enablePll);
	_ctrl.store(regs::pllControl, bits & ~pll_control::enablePll);
}

void Controller::programDpll(PllParams params) {
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

	_ctrl.store(regs::busMultiplier, bus_multiplier::vgaMultiplier(3)
			| bus_multiplier::dacMultiplier(3));
	
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

void Controller::enablePlane() {
	_ctrl.store(regs::planeOffset, 0);
	_ctrl.store(regs::planeStride, 640 * 4);
	_ctrl.store(regs::planeAddress, 0);

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

COFIBER_ROUTINE(cofiber::no_future, bindController(mbus::Entity entity), ([=] {
	protocols::hw::Device device(COFIBER_AWAIT entity.bind());
	auto info = COFIBER_AWAIT device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto ctrl_bar = COFIBER_AWAIT device.accessBar(0);
	auto memory_bar = COFIBER_AWAIT device.accessBar(2);
//	auto irq = COFIBER_AWAIT device.accessIrq();
	
	void *ctrl_window, *memory_window;
	HEL_CHECK(helMapMemory(ctrl_bar.getHandle(), kHelNullHandle, nullptr,
			0, 0x8'0000, kHelMapReadWrite | kHelMapShareAtFork, &ctrl_window));
	HEL_CHECK(helMapMemory(memory_bar.getHandle(), kHelNullHandle, nullptr,
			0, 0x1000'0000, kHelMapReadWrite | kHelMapShareAtFork, &memory_window));

	Controller controller{arch::mem_space(ctrl_window), memory_window};
	controller.run();
}))

COFIBER_ROUTINE(cofiber::no_future, observeControllers(), ([] {
	auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();

	auto filter = mbus::Conjunction({
		mbus::EqualsFilter("pci-vendor", "8086"),
		mbus::EqualsFilter("pci-device", "2e32")
	});
	COFIBER_AWAIT root.linkObserver(std::move(filter),
			[] (mbus::AnyEvent event) {
		if(event.type() == typeid(mbus::AttachEvent)) {
			std::cout << "gfx_intel: Detected controller" << std::endl;
			bindController(boost::get<mbus::AttachEvent>(event).getEntity());
		}else{
			throw std::runtime_error("Unexpected event type");
		}
	});
}))

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting Intel graphics driver\n");

	observeControllers();

	while(true)
		helix::Dispatcher::global().dispatch();
	
	return 0;
}
 
