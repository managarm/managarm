#pragma once

#include <netserver/phy.hpp>

namespace netserver::phy {

inline int linkSpeedToInt(nic::LinkSpeed speed) {
	switch (speed) {
		case nic::LinkSpeed::unknown:
			return -1;
		case nic::LinkSpeed::speed10:
			return 10;
		case nic::LinkSpeed::speed100:
			return 100;
		case nic::LinkSpeed::speed1000:
			return 1000;
		case nic::LinkSpeed::speed2500:
			return 2500;
		case nic::LinkSpeed::speed5000:
			return 5000;
		case nic::LinkSpeed::speed10000:
			return 10000;
	}
}

struct GenericEthernetPhy : nic::EthernetPhy {
	using EthernetPhy::EthernetPhy;

	async::result<nic::PhyResult<void>> configure() override;
	async::result<nic::PhyResult<void>> startup() override;

private:
	async::result<nic::PhyResult<void>> performAutoNegotiation();
	async::result<nic::PhyResult<void>> updateLink();
};

} // namespace netserver::phy
