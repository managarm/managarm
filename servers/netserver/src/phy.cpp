#include <print>

#include <frg/expected.hpp>
#include <helix/timer.hpp>
#include <netserver/phy.hpp>

namespace {

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

namespace rtl8211f {

constexpr uint32_t phyStatus = 0x1a;
constexpr uint32_t phyStatusSpeed = 0x0030;
constexpr uint32_t phyStatusSpeed1000 = 0x0020;
constexpr uint32_t phyStatusSpeed100 = 0x0010;
constexpr uint32_t phyStatusFullDuplex = 0x0008;
constexpr uint32_t phyStatusLink = 0x0004;

} // namespace rtl8211f

int linkSpeedToInt(nic::LinkSpeed speed) {
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

async::result<nic::PhyResult<void>> GenericEthernetPhy::configure() {
	FRG_CO_TRY(co_await mdio_->write(phyAddress_, miiBmcr, bmcr::reset));

	co_await performAutoNegotiation();

	co_return {};
}

async::result<nic::PhyResult<void>> GenericEthernetPhy::startup() {
	co_await updateLink();

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

} // namespace

namespace nic {

async::result<std::shared_ptr<EthernetPhy>>
makeEthernetPhy(std::shared_ptr<Mdio> mdio, uint8_t phyAddress) {
	auto physId1 = co_await mdio->read(phyAddress, 0x2);
	if (!physId1) {
		std::println("phy: Failed to read PHY ID1");
		co_return nullptr;
	}

	auto physId2 = co_await mdio->read(phyAddress, 0x3);
	if (!physId2) {
		std::println("phy: Failed to read PHY ID2");
		co_return nullptr;
	}

	auto physId = (*physId1 & 0xffff) << 16 | (*physId2 & 0xffff);
	switch (physId) {
		case 0x1cc916:
			std::println("phy: Found RTL8211F PHY");
			co_return std::make_shared<Rtl8211fPhy>(mdio, phyAddress);
		default:
			std::println("phy: Unknown PHY ID: {:#x}, using generic PHY driver", physId);
			co_return std::make_shared<GenericEthernetPhy>(mdio, phyAddress);
	}
}

} // namespace nic
