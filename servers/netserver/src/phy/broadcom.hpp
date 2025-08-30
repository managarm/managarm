#pragma once

#include <netserver/phy.hpp>

#include "generic.hpp"

namespace netserver::phy {

struct Bcm54210EPhy : GenericEthernetPhy {
	using GenericEthernetPhy::GenericEthernetPhy;

	async::result<nic::PhyResult<void>> configure() override;
	async::result<nic::PhyResult<void>> startup() override;

private:
	async::result<nic::PhyResult<uint16_t>> auxctlRead_(uint16_t reg);
	async::result<nic::PhyResult<void>> auxctlWrite_(uint16_t reg, uint16_t value);

	async::result<nic::PhyResult<uint16_t>> shadowRead_(uint16_t reg);
	async::result<nic::PhyResult<void>> shadowWrite_(uint16_t reg, uint16_t value);

	async::result<nic::PhyResult<void>> configureClockDelays_();
	async::result<nic::PhyResult<void>> enableWirespeed_();
	async::result<nic::PhyResult<void>> configureLeds_();
};

} // namespace netserver::phy
