#pragma once

#include <thor-internal/initgraph.hpp>
#include <thor-internal/arch/cpu.hpp>
#include <thor-internal/irq.hpp>
#include <arch/mem_space.hpp>

namespace thor {

struct GicCpuInterface;

struct GicDistributor {
	friend struct GicCpuInterface;

	GicDistributor(uintptr_t addr);

	void init();
	void initOnThisCpu();
	void sendIpi(uint8_t ifaceNo, uint8_t id);
	void sendIpiToOthers(uint8_t id);
	void dumpPendingSgis();

	frg::string<KernelAlloc> buildPinName(uint32_t irq);

	struct Pin : IrqPin {
		friend struct GicDistributor;

		Pin(GicDistributor *parent, uint32_t irq)
		: IrqPin{parent->buildPinName(irq)}, parent_{parent}, irq_{irq} {}

		IrqStrategy program(TriggerMode mode, Polarity polarity) override;
		void mask() override;
		void unmask() override;
		void sendEoi() override;

		void activate();
		void deactivate();

		bool setMode(TriggerMode trigger, Polarity polarity);

	private:
		void setAffinity_(uint8_t ifaceNo);
		void setPriority_(uint8_t prio);

		GicDistributor *parent_;
		uint32_t irq_;
	};

	Pin *setupIrq(uint32_t irq, TriggerMode mode);
	Pin *getPin(uint32_t irq) {
		if (irq >= irqPins_.size())
			return nullptr;

		return irqPins_[irq];
	}

private:
	uint8_t getCurrentCpuIfaceNo_();

	uintptr_t base_;
	arch::mem_space space_;
	frg::vector<Pin *, KernelAlloc> irqPins_;
};

struct GicCpuInterface {
	GicCpuInterface(GicDistributor *dist, uintptr_t addr, size_t size);

	void init();

	// returns {cpuId, irqId}
	frg::tuple<uint8_t, uint32_t> get();
	void eoi(uint8_t cpuId, uint32_t irqId);

	uint8_t getCurrentPriority();

	GicDistributor *getDistributor() const {
		return dist_;
	}

	uint8_t interfaceNumber() const {
		return ifaceNo_;
	}

private:
	GicDistributor *dist_;
	arch::mem_space space_;
	bool useSplitEoiDeact_;

	uint8_t ifaceNo_;
};

initgraph::Stage *getIrqControllerReadyStage();

void initGicOnThisCpu();

}
