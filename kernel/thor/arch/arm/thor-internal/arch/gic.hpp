#pragma once

#include <thor-internal/initgraph.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/irq.hpp>
#include <arch/mem_space.hpp>

namespace thor {

struct GicDistributor {
	GicDistributor(uintptr_t addr);

	void init();
	void initOnThisCpu();
	void sendIpi(uint8_t cpu, uint8_t id);
	void sendIpiToOthers(uint8_t id);

	frg::string<KernelAlloc> buildPinName(uint32_t irq);

	struct Pin : IrqPin {
		friend struct GicDistributor;

		Pin(GicDistributor *parent, uint32_t irq)
		: IrqPin{parent->buildPinName(irq)}, parent_{parent}, irq_{irq} {}

		IrqStrategy program(TriggerMode mode, Polarity) override;
		void mask() override;
		void unmask() override;
		void sendEoi() override;

	private:
		GicDistributor *parent_;
		uint32_t irq_;
	};

	Pin *setupIrq(uint32_t irq, TriggerMode mode);

	void configureTrigger(uint32_t irq, TriggerMode trigger);

private:
	uintptr_t base_;
	arch::mem_space space_;
	frg::vector<IrqPin *, KernelAlloc> irqPins_;
};

struct GicCpuInterface {
	GicCpuInterface(GicDistributor *dist, uintptr_t addr);

	void init();

	// returns {cpuId, irqId}
	frg::tuple<uint8_t, uint32_t> get();
	void eoi(uint8_t cpuId, uint32_t irqId);

private:
	GicDistributor *dist_;
	arch::mem_space space_;
};

initgraph::Stage *getIrqControllerReadyStage();

void initGicOnThisCpu();

}
