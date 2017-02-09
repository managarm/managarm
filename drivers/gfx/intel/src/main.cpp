
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
	static constexpr arch::bit_register<uint32_t> pllDivisor1(0x6040);
	static constexpr arch::bit_register<uint32_t> pllDivisor2(0x6044);
	
	static constexpr arch::bit_register<uint32_t> dacPort(0x61100);
	
	static constexpr arch::bit_register<uint32_t> pipeConfig(0x70008);

	static constexpr arch::bit_register<uint32_t> planeControl(0x70180);
}

namespace pll_control {
	static constexpr arch::field<uint32_t, unsigned int> phase(9, 4);
	static constexpr arch::field<uint32_t, unsigned int> encodedP1(16, 8);
	static constexpr arch::field<uint32_t, unsigned int> modeSelect(27, 2);
	static constexpr arch::field<uint32_t, bool> disableVga(28, 1);
	static constexpr arch::field<uint32_t, bool> enableVco(31, 1);
}

namespace pll_divisor {
	static constexpr arch::field<uint32_t, unsigned int> m2(0, 6);
	static constexpr arch::field<uint32_t, unsigned int> m1(8, 6);
	static constexpr arch::field<uint32_t, unsigned int> n(16, 6);
}

namespace dac_port {
	static constexpr arch::field<uint32_t, bool> enableDac(31, 1);
}

namespace pipe_config {
	static constexpr arch::field<uint32_t, bool> enablePipe(31, 1);
	static constexpr arch::field<uint32_t, bool> pipeStatus(30, 1);
}

namespace plane_control {
	static constexpr arch::field<uint32_t, bool> enablePlane(31, 1);
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
constexpr PllLimits limitsG4x {
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

struct Controller {
	Controller(arch::mem_space space)
	: _space{space} { }

	void run();

	void disableDac();
	void enableDac();

	void disablePlane();

	void disablePipe();
	void enablePipe();

private:
	arch::mem_space _space;
};

void Controller::disableDac() {
	auto bits = _space.load(regs::dacPort);
	std::cout << "DAC Port: " << static_cast<uint32_t>(bits) << std::endl;
	assert(bits & dac_port::enableDac);
	_space.store(regs::dacPort, bits & ~dac_port::enableDac);
}

void Controller::enableDac() {
	auto bits = _space.load(regs::dacPort);
	assert(!(bits & dac_port::enableDac));
	_space.store(regs::dacPort, bits | dac_port::enableDac(true));
}

void Controller::disablePlane() {
	auto bits = _space.load(regs::planeControl);
	std::cout << "Plane control: " << static_cast<uint32_t>(bits) << std::endl;
	assert(bits & plane_control::enablePlane);
	_space.store(regs::planeControl, bits & ~plane_control::enablePlane);
}

void Controller::disablePipe() {
	auto bits = _space.load(regs::pipeConfig);
	std::cout << "Pipe config: " << static_cast<uint32_t>(bits) << std::endl;
	assert(bits & pipe_config::enablePipe);
	assert(bits & pipe_config::pipeStatus);
	_space.store(regs::pipeConfig, bits & ~pipe_config::enablePipe);
	
	std::cout << "After disable: " << (_space.load(regs::pipeConfig)
			& pipe_config::pipeStatus) << std::endl;
	while(_space.load(regs::pipeConfig) & pipe_config::pipeStatus) {
		// Busy wait until the pipe is shut off.
	}

	std::cout << "Pipe disabled" << std::endl;
}

void Controller::enablePipe() {
	auto bits = _space.load(regs::pipeConfig);
	assert(!(bits & pipe_config::enablePipe));
	assert(!(bits & pipe_config::pipeStatus));
	_space.store(regs::pipeConfig, bits | pipe_config::enablePipe(true));
	
	while(!(_space.load(regs::pipeConfig) & pipe_config::pipeStatus)) {
		// Busy wait until the pipe is ready.
	}

	std::cout << "Pipe enabled" << std::endl;
}

void Controller::run() {
/*
	auto control = _space.load(regs::vgaPllPost);
	auto divisor1 = _space.load(regs::vgaPllDivisor1);
	auto divisor2 = _space.load(regs::vgaPllDivisor2);
	std::cout << static_cast<uint32_t>(control)
			<< " " << static_cast<uint32_t>(divisor1)
			<< " " << static_cast<uint32_t>(divisor2) << std::endl;
	
	PllParams params;
	params.n = divisor1 & pll_divisor::n;
	params.m1 = divisor1 & pll_divisor::m1;
	params.m2 = divisor1 & pll_divisor::m2;
	params.p1 = __builtin_ffs(control & pll_control::encodedP1);
	params.p2 = 10;

	std::cout << "valid: " << checkParams(params, 96'000, limitsG4x) << std::endl;
	std::cout << "n: " << params.n << ", m1: " << params.m1 << ", m2: " << params.m2
			<< ", p1: " << params.p1 << ", p2: " << params.p2 << std::endl;
	std::cout << "dot: " << params.computeDot(96'000) << std::endl;
	std::cout << "vco: " << params.computeVco(96'000) << std::endl;
	std::cout << "m: " << params.computeM() << std::endl;
	std::cout << "p: " << params.computeP() << std::endl;
	
	params.n = divisor2 & pll_divisor::n;
	params.m1 = divisor2 & pll_divisor::m1;
	params.m2 = divisor2 & pll_divisor::m2;
	params.p1 = __builtin_ffs(control & pll_control::encodedP1);
	params.p2 = 10;

	std::cout << "valid: " << checkParams(params, 96'000, limitsG4x) << std::endl;
	std::cout << "n: " << params.n << ", m1: " << params.m1 << ", m2: " << params.m2
			<< ", p1: " << params.p1 << ", p2: " << params.p2 << std::endl;
	std::cout << "dot: " << params.computeDot(96'000) << std::endl;
	std::cout << "vco: " << params.computeVco(96'000) << std::endl;
	std::cout << "m: " << params.computeM() << std::endl;
	std::cout << "p: " << params.computeP() << std::endl;
*/
	disableDac();
	//disablePlane();
	disablePipe();
	enablePipe();
	enableDac();
}

// ----------------------------------------------------------------
// Freestanding PCI discovery functions.
// ----------------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, bindController(mbus::Entity entity), ([=] {
	protocols::hw::Device device(COFIBER_AWAIT entity.bind());
	auto info = COFIBER_AWAIT device.getPciInfo();
	assert(info.barInfo[0].ioType == protocols::hw::IoType::kIoTypeMemory);
	auto bar = COFIBER_AWAIT device.accessBar(0);
//	auto irq = COFIBER_AWAIT device.accessIrq();
	
	void *actual_pointer;
	HEL_CHECK(helMapMemory(bar.getHandle(), kHelNullHandle, nullptr,
			0, 0x80000, kHelMapReadWrite | kHelMapShareAtFork, &actual_pointer));

	Controller controller{arch::mem_space(actual_pointer)};
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
 
