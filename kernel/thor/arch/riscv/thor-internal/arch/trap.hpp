#pragma once

#include <thor-internal/arch-generic/cpu.hpp>

namespace thor {

extern "C" void thorExceptionEntry();

void handleRiscvWorkOnExecutor(Executor *executor, Frame *frame);

enum ExternalIrqType {
	none,
	plic,
	aplic,
};

struct ExternalIrq {
	ExternalIrqType type{ExternalIrqType::none};
	void *controller{nullptr};
	// For PLIC: index of the PLIC context.
	// For APLIC: hart index inside the APLIC domain.
	size_t context{~size_t{0}};
};

extern PerCpu<ExternalIrq> riscvExternalIrq;

IrqPin *claimPlicIrq();
IrqPin *claimAplicIrq();

} // namespace thor
