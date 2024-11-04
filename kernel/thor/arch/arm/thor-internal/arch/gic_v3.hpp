#pragma once

#include <frg/manual_box.hpp>
#include <thor-internal/arch/gic.hpp>

namespace thor {

struct GicDistributorV3;

struct GicDistributorV3 {
	friend struct GicPinV3;

	GicDistributorV3(uintptr_t addr, uintptr_t size);

	void init();

	frg::string<KernelAlloc> buildPinName(uint32_t irq);

  private:
	friend struct GicV3;
	friend struct GicPinV3;

	uintptr_t base_;
	arch::mem_space space_;
};

struct GicRedistributorV3 {
	constexpr GicRedistributorV3() : space_{} {}
	GicRedistributorV3(arch::mem_space space);

	void initOnThisCpu();
	bool ownedBy(uint32_t affinity) const;

  private:
	friend struct GicPinV3;

	arch::mem_space space_;
};

struct GicPinV3 : public Gic::Pin {
	GicPinV3(GicDistributorV3 *dist, uint32_t irq) : Gic::Pin{dist->buildPinName(irq)}, irq_{irq} {}

	bool setMode(TriggerMode trigger, Polarity polarity) override;

	IrqStrategy program(TriggerMode mode, Polarity polarity) override;

	void mask() override;
	void unmask() override;

	void sendEoi() override;

  private:
	friend struct GicV3;
	friend void initGicOnThisCpuV3();

	void setAffinity_(uint32_t affinity);
	void setPriority_(uint8_t priority);

	uint32_t irq_;
};

struct GicV3 : public Gic {
	GicV3();

	void sendIpi(int cpuId, uint8_t id) override;
	void sendIpiToOthers(uint8_t id) override;

	CpuIrq getIrq() override;
	void eoi(uint32_t cpuId, uint32_t id) override;

	Pin *setupIrq(uint32_t irq, TriggerMode trigger) override;
	Pin *getPin(uint32_t irq) override;

  private:
	frg::vector<GicPinV3 *, KernelAlloc> irqPins_;
};

bool initGicV3();
void initGicOnThisCpuV3();

} // namespace thor
