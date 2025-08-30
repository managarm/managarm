#include <print>

#include <frg/expected.hpp>
#include <helix/timer.hpp>
#include <netserver/phy.hpp>

#include "realtek.hpp"

namespace netserver::phy {

namespace rtl8211f {

constexpr uint32_t phyStatus = 0x1a;
constexpr uint32_t phyStatusSpeed = 0x0030;
constexpr uint32_t phyStatusSpeed1000 = 0x0020;
constexpr uint32_t phyStatusSpeed100 = 0x0010;
constexpr uint32_t phyStatusFullDuplex = 0x0008;
constexpr uint32_t phyStatusLink = 0x0004;

} // namespace rtl8211f


async::result<nic::PhyResult<void>> Rtl8211fPhy::configure() {
	// Set green LED for link, yellow LED for active
	{
		FRG_CO_TRY(co_await switchPage(0xd04));
		// Set LED configuration
		FRG_CO_TRY(co_await mdio_->write(phyAddress_, 0x10, 0x617f));
		FRG_CO_TRY(co_await switchPage(0));
	}

	// Call the base class configure method.
	co_await GenericEthernetPhy::configure();

	co_return {};
}

async::result<nic::PhyResult<void>> Rtl8211fPhy::startup() {
	// Perform the PHY startup sequence.
	co_await GenericEthernetPhy::startup();

	if (!linkStatus_) {
		co_return {};
	}

	FRG_CO_TRY(co_await switchPage(0xa43));

	auto phyStatus = FRG_CO_TRY(co_await mdio_->read(phyAddress_, rtl8211f::phyStatus));

	while (!(phyStatus & rtl8211f::phyStatusLink)) {
		co_await helix::sleepFor(100'000'000); // 100ms

		phyStatus = FRG_CO_TRY(co_await mdio_->read(phyAddress_, rtl8211f::phyStatus));
	}

	if (phyStatus & rtl8211f::phyStatusFullDuplex) {
		duplex_ = nic::LinkDuplex::full;
	} else {
		duplex_ = nic::LinkDuplex::half;
	}

	switch (phyStatus & rtl8211f::phyStatusSpeed) {
		case rtl8211f::phyStatusSpeed1000:
			speed_ = nic::LinkSpeed::speed1000;
			break;
		case rtl8211f::phyStatusSpeed100:
			speed_ = nic::LinkSpeed::speed100;
			break;
		default:
			speed_ = nic::LinkSpeed::speed10;
			break;
	}

	FRG_CO_TRY(co_await switchPage(0));

	std::println(
	    "rtl8211f: Speed: {}Mbps, duplex: {}",
	    linkSpeedToInt(speed_),
	    duplex_ == nic::LinkDuplex::full ? "full" : "half"
	);

	co_return {};
}

} // namespace netserver::phy
