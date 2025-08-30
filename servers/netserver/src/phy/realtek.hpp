#pragma once

#include <frg/expected.hpp>
#include <netserver/phy.hpp>

#include "generic.hpp"

namespace netserver::phy {

struct Rtl8211fPhy : GenericEthernetPhy {
	using GenericEthernetPhy::GenericEthernetPhy;

	async::result<nic::PhyResult<void>> configure() override;
	async::result<nic::PhyResult<void>> startup() override;

private:
	async::result<nic::PhyResult<void>> switchPage(uint16_t page) {
		FRG_CO_TRY(co_await mdio_->write(phyAddress_, 0x1f, page));
		co_return {};
	}
};

} // namespace netserver::phy
