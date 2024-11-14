#pragma once

#include <initgraph.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/arch/gic.hpp>
#include <thor-internal/irq.hpp>
#include <arch/mem_space.hpp>

namespace thor {

struct GicCpuInterfaceV2;

struct GicDistributorV2 {
	friend struct GicCpuInterfaceV2;

	GicDistributorV2(uintptr_t addr);

	void init();
	void initOnThisCpu();
	void sendIpi(uint8_t ifaceNo, uint8_t id);
	void sendIpiToOthers(uint8_t id);
	void dumpPendingSgis();

	frg::string<KernelAlloc> buildPinName(uint32_t irq);

	struct Pin : Gic::Pin {
		friend struct GicDistributorV2;

		Pin(GicDistributorV2 *parent, uint32_t irq)
		: Gic::Pin{parent->buildPinName(irq)}, parent_{parent}, irq_{irq} {}

		IrqStrategy program(TriggerMode mode, Polarity polarity) override;
		void mask() override;
		void unmask() override;
		void sendEoi() override;

		void activate();
		void deactivate();

		bool setMode(TriggerMode trigger, Polarity polarity) override;

	private:
		void setAffinity_(uint8_t ifaceNo);
		void setPriority_(uint8_t prio);

		GicDistributorV2 *parent_;
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

struct GicCpuInterfaceV2 {
	GicCpuInterfaceV2(GicDistributorV2 *dist, uintptr_t addr, size_t size);

	void init();

	// returns {cpuId, irqId}
	frg::tuple<uint8_t, uint32_t> get();
	void eoi(uint8_t cpuId, uint32_t irqId);

	uint8_t getCurrentPriority();

	GicDistributorV2 *getDistributor() const {
		return dist_;
	}

	uint8_t interfaceNumber() const {
		return ifaceNo_;
	}

private:
	GicDistributorV2 *dist_;
	arch::mem_space space_;
	bool useSplitEoiDeact_;

	uint8_t ifaceNo_;
};

struct GicV2 : public Gic {
	void sendIpi(int cpuId, uint8_t id) override;
	void sendIpiToOthers(uint8_t id) override;

	CpuIrq getIrq() override;
	void eoi(uint32_t cpuId, uint32_t id) override;

	Pin *setupIrq(uint32_t irq, TriggerMode trigger) override;
	Pin *getPin(uint32_t irq) override;
};

bool initGicV2();
void initGicOnThisCpuV2();

}
