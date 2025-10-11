#pragma once

#include <variant>

#include <initgraph.hpp>
#include <thor-internal/cpu-data.hpp>
#include <thor-internal/irq.hpp>

namespace thor {

using ExternalIrqController = std::variant<std::monostate, struct GicV2 *, struct GicV3 *>;

struct ClaimedExternalIrq {
	uint32_t cpu{0};
	uint32_t irq{0};
	IrqPin *pin{nullptr};
};

extern ExternalIrqController externalIrq;

ClaimedExternalIrq claimGicV2Irq();
ClaimedExternalIrq claimGicV3Irq();

initgraph::Stage *getIrqControllerReadyStage();

void initializeIrqVectors();

} // namespace thor
