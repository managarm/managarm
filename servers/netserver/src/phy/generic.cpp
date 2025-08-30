#include <print>

#include <frg/expected.hpp>
#include <helix/timer.hpp>
#include <netserver/phy.hpp>

#include "generic.hpp"

namespace netserver::phy {

enum : uint8_t {
	miiBmcr = 0, // Basic Mode Control Register
	miiBmsr = 1, // Basic Mode Status Register
	miiAnar = 4, // Auto-Negotiation Advertisement Register
};

namespace bmcr {

constexpr uint16_t speed100 = 1 << 6;    // Force 100 Mbps
constexpr uint16_t fullDuplex = 1 << 8;  // Force Full Duplex
constexpr uint16_t restartAneg = 1 << 9; // Restart Auto-Negotiation
constexpr uint16_t isolate = 1 << 10;    // Isolate PHY from MII
constexpr uint16_t enableAneg = 1 << 12; // Enable Auto-Negotiation
constexpr uint16_t speed1000 = 1 << 13;  // Force 1000 Mbps
constexpr uint16_t reset = 1 << 15;      // Reset the PHY

} // namespace bmcr

namespace bmsr {

constexpr uint16_t linkStatus = 1 << 2;   // Link Status
constexpr uint16_t anegComplete = 1 << 5; // Auto-Negotiation Complete

} // namespace bmsr


async::result<nic::PhyResult<void>> GenericEthernetPhy::configure() {
	FRG_CO_TRY(co_await mdio_->write(phyAddress_, miiBmcr, bmcr::reset));

	co_return {};
}

async::result<nic::PhyResult<void>> GenericEthernetPhy::startup() {
	FRG_CO_TRY(co_await performAutoNegotiation());
	FRG_CO_TRY(co_await updateLink());

	co_return {};
}

async::result<nic::PhyResult<void>> GenericEthernetPhy::performAutoNegotiation() {
	if (!autoNegotiate_) {
		// If auto-negotiation is disabled, we just set the link speed and duplex mode.
		std::println("generic-phy: Auto-negotiation is disabled, setting link speed and duplex mode"
		);
		std::println(
		    "generic-phy: Speed: {}Mbps, duplex: {}",
		    linkSpeedToInt(speed_),
		    duplex_ == nic::LinkDuplex::full ? "full" : "half"
		);

		uint16_t value = 0;

		if (speed_ == nic::LinkSpeed::speed1000)
			value |= bmcr::speed1000;
		else if (speed_ == nic::LinkSpeed::speed100)
			value |= bmcr::speed100;

		if (duplex_ == nic::LinkDuplex::full)
			value |= bmcr::fullDuplex;

		FRG_CO_TRY(co_await mdio_->write(phyAddress_, miiBmcr, value));
	} else {
		std::println("generic-phy: Performing auto-negotiation");

		auto bmcr = FRG_CO_TRY(co_await mdio_->read(phyAddress_, miiBmcr));

		bmcr |= bmcr::enableAneg | bmcr::restartAneg;
		bmcr &= ~bmcr::isolate;

		FRG_CO_TRY(co_await mdio_->write(phyAddress_, miiBmcr, bmcr));
	}

	co_return {};
}

async::result<nic::PhyResult<void>> GenericEthernetPhy::updateLink() {
	auto bmsr = FRG_CO_TRY(co_await mdio_->read(phyAddress_, miiBmsr));

	// If the link is up, we don't need to do auto-negotiation.
	if (linkStatus_ && bmsr & bmsr::linkStatus) {
		co_return {};
	}

	if (autoNegotiate_ && !(bmsr & bmsr::anegComplete)) {
		// Let's wait for auto-negotiation to complete.
		std::println("generic-phy: Waiting for auto-negotiation to complete");

		uint64_t startNs;
		HEL_CHECK(helGetClock(&startNs));

		while (!(bmsr & bmsr::anegComplete)) {
			uint64_t currNs;
			HEL_CHECK(helGetClock(&currNs));

			if (currNs - startNs > 10'000'000'000) { // 10 seconds
				std::println("generic-phy: Auto-negotiation timed out after 10 seconds");

				linkStatus_ = false;
				speed_ = nic::LinkSpeed::unknown;
				duplex_ = nic::LinkDuplex::unknown;

				co_return {};
			}

			bmsr = FRG_CO_TRY(co_await mdio_->read(phyAddress_, miiBmsr));

			co_await helix::sleepFor(500'000'000); // 500ms
		}

		uint64_t currNs;
		HEL_CHECK(helGetClock(&currNs));

		std::println(
		    "generic-phy: Auto-negotiation complete in {}ms", (currNs - startNs) / 1'000'000
		);
	}

	bmsr = FRG_CO_TRY(co_await mdio_->read(phyAddress_, miiBmsr));
	linkStatus_ = bmsr & bmsr::linkStatus;

	std::println("generic-phy: Link is {}", linkStatus_ ? "up" : "down");

	co_return {};
}

} // namespace netserver::phy
