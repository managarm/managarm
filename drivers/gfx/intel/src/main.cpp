
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

namespace regs {
	static constexpr arch::bit_register<uint32_t> vgaPllDivisor1(0x6000);
	static constexpr arch::bit_register<uint32_t> vgaPllDivisor2(0x6004);
	static constexpr arch::bit_register<uint32_t> vgaPllPost(0x6010);
	static constexpr arch::bit_register<uint32_t> pllControl(0x6014);
	static constexpr arch::bit_register<uint32_t> busMultiplier(0x601C);
	static constexpr arch::bit_register<uint32_t> pllDivisor1(0x6040);
	static constexpr arch::bit_register<uint32_t> pllDivisor2(0x6044);
	
	static constexpr arch::bit_register<uint32_t> htotal(0x60000);
	static constexpr arch::bit_register<uint32_t> hblank(0x60004);
	static constexpr arch::bit_register<uint32_t> hsync(0x60008);
	static constexpr arch::bit_register<uint32_t> vtotal(0x6000C);
	static constexpr arch::bit_register<uint32_t> vblank(0x60010);
	static constexpr arch::bit_register<uint32_t> vsync(0x60014);
	static constexpr arch::bit_register<uint32_t> sourceSize(0x6001C);

	static constexpr arch::bit_register<uint32_t> dacPort(0x61100);
	
	static constexpr arch::bit_register<uint32_t> pipeConfig(0x70008);

	static constexpr arch::bit_register<uint32_t> planeControl(0x70180);
	static constexpr arch::scalar_register<uint32_t> planeOffset(0x70184);
	static constexpr arch::scalar_register<uint32_t> planeStride(0x70188);
	static constexpr arch::scalar_register<uint32_t> planeAddress(0x7019C);

	static constexpr arch::bit_register<uint32_t> vgaControl(0x71400);
}

namespace pll_control {
	static constexpr arch::field<uint32_t, unsigned int> phase(9, 4);
	static constexpr arch::field<uint32_t, unsigned int> encodedP1(16, 8);
	static constexpr arch::field<uint32_t, unsigned int> modeSelect(26, 2);
	static constexpr arch::field<uint32_t, bool> disableVga(28, 1);
	static constexpr arch::field<uint32_t, bool> enablePll(31, 1);
}

namespace bus_multiplier {
	static constexpr arch::field<uint32_t, unsigned int> vgaMultiplier(0, 6);
	static constexpr arch::field<uint32_t, unsigned int> dacMultiplier(8, 6);
}

namespace pll_divisor {
	static constexpr arch::field<uint32_t, unsigned int> m2(0, 6);
	static constexpr arch::field<uint32_t, unsigned int> m1(8, 6);
	static constexpr arch::field<uint32_t, unsigned int> n(16, 6);
}

namespace hvtotal {
	static constexpr arch::field<uint32_t, unsigned int> active(0, 12);
	static constexpr arch::field<uint32_t, unsigned int> total(16, 13);
}

namespace hvblank {
	static constexpr arch::field<uint32_t, unsigned int> start(0, 13);
	static constexpr arch::field<uint32_t, unsigned int> end(16, 13);
}

namespace hvsync {
	static constexpr arch::field<uint32_t, unsigned int> start(0, 13);
	static constexpr arch::field<uint32_t, unsigned int> end(16, 13);
}

namespace source_size {
	static constexpr arch::field<uint32_t, unsigned int> horizontal(16, 12);
	static constexpr arch::field<uint32_t, unsigned int> vertical(0, 12);
}

namespace dac_port {
	static constexpr arch::field<uint32_t, bool> enableDac(31, 1);
}

namespace pipe_config {
	static constexpr arch::field<uint32_t, bool> pipeStatus(30, 1);
	static constexpr arch::field<uint32_t, bool> enablePipe(31, 1);
}

enum class PrimaryFormat : unsigned int {
	INDEXED = 2,
	BGRX8888 = 6,
	RGBX8888 = 14
};

namespace plane_control {
	static constexpr arch::field<uint32_t, bool> enablePlane(31, 1);
	static constexpr arch::field<uint32_t, PrimaryFormat> pixelFormat(26, 4);
}

namespace vga_control {
	static constexpr arch::field<uint32_t, bool> disableVga(31, 1);
	static constexpr arch::field<uint32_t, unsigned int> centeringMode(30, 2);
}

struct PllLimits {
	struct {
		int min, max;
	} dot, vco, n, m, m1, m2, p, p1;

	struct {
		int dot_limit;
		int slow, fast;
	} p2;
};

// Note: These limits come from the Linux kernel.
// Strangly the G45 manual has a different set of limits.
constexpr PllLimits limitsG45 {
	{ 25'000, 270'000 },
	{ 1'750'000, 3'500'000 },
	{ 1, 4 },
	{ 104, 138 },
	{ 17, 23 },
	{ 5, 11 },
	{ 10, 30 },
	{ 1, 3 },
	{ 270'000, 10, 10 }
};

struct PllParams {
	int computeDot(int refclock) {
		auto p = computeP();
		return (computeVco(refclock) + p / 2) / p;
	}
	
	int computeVco(int refclock) {
		auto m = computeM();
		return (refclock * m + (n + 2) / 2) / (n + 2);
	}

	int computeM() {
		return 5 * (m1 + 2) + (m2 + 2);
	}

	int computeP() {
		return p1 * p2;
	}

	void dump(int refclock) {
		std::cout << "n: " << n << ", m1: " << m1 << ", m2: " << m2
				<< ", p1: " << p1 << ", p2: " << p2 << std::endl;
		std::cout << "m: " << computeM()
				<< ", p: " << computeP() << std::endl;
		std::cout << "dot: " << computeDot(refclock)
				<< ", vco: " << computeVco(refclock) << std::endl;
	}
	
	int n, m1, m2, p1, p2;
};

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

struct Timings {
	int active;
	int syncStart;
	int syncEnd;
	int total;

	int blankingStart() {
		return active;
	}
	int blankingEnd() {
		return total;
	}

	void dump() {
		std::cout << "active: " << active << ", start of sync: " << syncStart
				<< ", end of sync: " << syncEnd << ", total: " << total << std::endl;
	}
};

struct Mode {
	int dot;
	Timings horizontal;
	Timings vertical;
};

struct Controller {
	Controller(arch::mem_space ctrl, void *memory)
	: _ctrl{ctrl}, _memory{memory} { }

	void run();

	void disableDac();
	void enableDac();

	void disablePlane();
	void enablePlane();

	void disablePipe();
	void programPipe(Mode mode);

	void disableDpll();
	void programDpll(PllParams params);

	void relinquishVga();

	void dumpDpll();
	void dumpPipe();

private:
	arch::mem_space _ctrl;
	void *_memory;
};

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

void Controller::relinquishVga() {
	auto bits = _ctrl.load(regs::vgaControl);
	assert(!(bits & vga_control::disableVga));
	_ctrl.store(regs::vgaControl, (bits & ~vga_control::centeringMode)
			| vga_control::disableVga(true));
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
 
