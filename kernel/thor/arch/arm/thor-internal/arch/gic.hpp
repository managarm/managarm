#pragma once

#include <initgraph.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/irq.hpp>
#include <arch/mem_space.hpp>

namespace thor {

struct Gic {
	virtual void sendIpi(int cpuId, uint8_t id) = 0;
	virtual void sendIpiToOthers(uint8_t id) = 0;

	struct CpuIrq {
		uint32_t cpu;
		uint32_t irq;
	};

	virtual CpuIrq getIrq() = 0;
	virtual void eoi(uint32_t cpuId, uint32_t id) = 0;

	struct Pin : public IrqPin {
		virtual ~Pin() = default;

		Pin(frg::string<KernelAlloc> name)
		: IrqPin{std::move(name)} {}

		virtual bool setMode(TriggerMode trigger, Polarity polarity) = 0;

		virtual IrqStrategy program(TriggerMode mode, Polarity polarity) override = 0;

		virtual void mask() override = 0;
		virtual void unmask() override = 0;

		virtual void sendEoi() override = 0;
	};

	virtual Pin *setupIrq(uint32_t irq, TriggerMode trigger) = 0;
	virtual Pin *getPin(uint32_t irq) = 0;
};

initgraph::Stage *getIrqControllerReadyStage();

void initGicOnThisCpu();

extern Gic *gic;

}
