#pragma once

#include <initgraph.hpp>
#include <thor-internal/arch-generic/cpu.hpp>
#include <thor-internal/dtb/irq.hpp>
#include <thor-internal/irq.hpp>
#include <arch/mem_space.hpp>

namespace thor {

struct Gic : dt::IrqController {
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

	IrqPin *resolveDtIrq(dtb::Cells irqSpecifier) override {
		if (irqSpecifier.numCells() != 3 && irqSpecifier.numCells() != 4)
			panicLogger() << "GIC #interrupt-cells should be 3 or 4" << frg::endlog;
		uint32_t type;
		if (!irqSpecifier.readSlice(type, 0, 1))
			panicLogger() << "Failed to read GIC interrupt type" << frg::endlog;
		uint32_t idx;
		if (!irqSpecifier.readSlice(idx, 1, 1))
			panicLogger() << "Failed to read GIC interrupt index" << frg::endlog;
		uint32_t flags;
		if (!irqSpecifier.readSlice(flags, 2, 1))
			panicLogger() << "Failed to read GIC interrupt flags" << frg::endlog;
		frg::optional<uint32_t> ppiHandle = 0;
		if (!irqSpecifier.readSlice(*ppiHandle, 3, 1))
			ppiHandle = frg::null_opt;

		// TODO(qookie): Handle extended PPI and SPI.
		if (type != 0 && type != 1)
			panicLogger() << "Unexpected GIC interrupt type " << type << frg::endlog;

		TriggerMode trigger;
		Polarity polarity;

		switch (flags & 0xF) {
			case 1:
				polarity = Polarity::high;
				trigger = TriggerMode::edge;
				break;
			case 2:
				polarity = Polarity::low;
				trigger = TriggerMode::edge;
				break;
			case 4:
				polarity = Polarity::high;
				trigger = TriggerMode::level;
				break;
			case 8:
				polarity = Polarity::low;
				trigger = TriggerMode::level;
				break;
			default:
				infoLogger()
					<< "thor: Illegal IRQ flags " << (flags & 0xF)
					<< " found when parsing GIC interrupt"
					<< frg::endlog;
				polarity = Polarity::null;
				trigger = TriggerMode::null;
		}

		auto irq = idx + (type == 1 ? 16 : 32);

		// TODO(qookie): Care about polarity in some way?
		// AFAICT the GIC does not support configuring IRQ
		// polarity.
		(void)polarity;
		auto pin = setupIrq(irq, trigger);
		return pin;
	}
};

initgraph::Stage *getIrqControllerReadyStage();

void initGicOnThisCpu();

extern Gic *gic;

}
