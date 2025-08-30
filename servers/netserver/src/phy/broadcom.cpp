#include <print>

#include <frg/expected.hpp>
#include <helix/timer.hpp>
#include <netserver/phy.hpp>

#include "broadcom.hpp"

// Based off of the FreeBSD brgphy driver, and the Linux broadcom PHY driver (for the LED config).
// Heavily stripped down to only support BCM54210E (needed for RPi4).

namespace netserver::phy {

namespace bcm {

inline constexpr uint8_t dspRwPort = 0x15;
inline constexpr uint8_t dspAddr = 0x17;

inline constexpr uint8_t auxctl = 0x18;
inline constexpr uint16_t auxctlShadowSelMask = 0x0007;

inline constexpr uint16_t auxctlShadowSelMisc = 0x0007;
inline constexpr uint16_t auxctlShadowSelMiscWrEn = 0x8000;
inline constexpr uint16_t auxctlShadowSelMiscWirespeed = 0x0010;
inline constexpr uint16_t auxctlShadowSelMiscRxcSkewEn = 0x0100;


inline constexpr uint8_t shadow = 0x1c;

inline constexpr uint16_t shadowWrEn = 0x8000;
inline constexpr uint16_t shadowDataMask = 0x3ff;

inline constexpr uint16_t shadowClkCtrl = 0x03;
inline constexpr uint16_t shadowClkCtrlGTxClkEn = 0x200;

inline constexpr uint16_t shadowLeds1 = 0x0d;
inline constexpr uint16_t shadowLeds1Led13Multicolor1 = 0xaa;


inline constexpr uint8_t expSel = 0x17;
inline constexpr uint8_t expData = 0x15;

inline constexpr uint8_t expMulticolor = 0x0f04;
inline constexpr uint8_t expMulticolorInPhase = 0x0100;
inline constexpr uint8_t expMulticolorLed13LinkAct = 0x0000;


inline constexpr uint8_t auxsts = 0x19;

inline constexpr uint16_t auxstsAnegMask = 0x0700;
inline constexpr uint16_t auxstsAneg10HD = 0x0100;
inline constexpr uint16_t auxstsAneg10FD = 0x0200;
inline constexpr uint16_t auxstsAneg100HD = 0x0300;
inline constexpr uint16_t auxstsAneg100T4 = 0x0400;
inline constexpr uint16_t auxstsAneg100FD = 0x0500;
inline constexpr uint16_t auxstsAneg1000HD = 0x0600;
inline constexpr uint16_t auxstsAneg1000FD = 0x0700;

} // namespace bcm


async::result<nic::PhyResult<void>> Bcm54210EPhy::configure() {
	// Perform a generic PHY reset.
	FRG_CO_TRY(co_await GenericEthernetPhy::configure());

	FRG_CO_TRY(co_await configureClockDelays_());
	FRG_CO_TRY(co_await enableWirespeed_());
	FRG_CO_TRY(co_await configureLeds_());

	co_return {};
}

async::result<nic::PhyResult<void>> Bcm54210EPhy::startup() {
	// Perform a generic PHY startup.
	FRG_CO_TRY(co_await GenericEthernetPhy::startup());

	// Get the negotiated link speed
	if (!linkStatus_) {
		co_return {};
	}

	auto aux = FRG_CO_TRY(co_await mdio_->read(phyAddress_, bcm::auxsts));

	switch (aux & bcm::auxstsAnegMask) {
		case bcm::auxstsAneg1000FD:
			speed_ = nic::LinkSpeed::speed1000;
			duplex_ = nic::LinkDuplex::full;
			break;
		case bcm::auxstsAneg1000HD:
			speed_ = nic::LinkSpeed::speed1000;
			duplex_ = nic::LinkDuplex::half;
			break;
		case bcm::auxstsAneg100FD:
			speed_ = nic::LinkSpeed::speed100;
			duplex_ = nic::LinkDuplex::full;
			break;
		case bcm::auxstsAneg100T4:
			speed_ = nic::LinkSpeed::speed100;
			duplex_ = nic::LinkDuplex::half; // ?
			break;
		case bcm::auxstsAneg100HD:
			speed_ = nic::LinkSpeed::speed100;
			duplex_ = nic::LinkDuplex::half;
			break;
		case bcm::auxstsAneg10FD:
			speed_ = nic::LinkSpeed::speed10;
			duplex_ = nic::LinkDuplex::full;
			break;
		case bcm::auxstsAneg10HD:
			speed_ = nic::LinkSpeed::speed10;
			duplex_ = nic::LinkDuplex::half;
			break;
	}

	std::println(
		"Bcm54210EPhy: Speed: {}Mbps, duplex: {}",
		linkSpeedToInt(speed_),
		duplex_ == nic::LinkDuplex::full ? "full" : "half"
	);

	co_return {};
}

async::result<nic::PhyResult<uint16_t>> Bcm54210EPhy::auxctlRead_(uint16_t reg) {
	FRG_CO_TRY(co_await mdio_->write(phyAddress_, bcm::auxctl,
					(reg << 12) | bcm::auxctlShadowSelMask));

	co_return FRG_CO_TRY(co_await mdio_->read(phyAddress_, bcm::auxctl));
}

async::result<nic::PhyResult<void>> Bcm54210EPhy::auxctlWrite_(uint16_t reg, uint16_t value) {
	FRG_CO_TRY(co_await mdio_->write(phyAddress_, bcm::auxctl, reg | value));

	co_return {};
}


async::result<nic::PhyResult<uint16_t>> Bcm54210EPhy::shadowRead_(uint16_t reg) {
	FRG_CO_TRY(co_await mdio_->write(phyAddress_, bcm::shadow, reg << 10));

	co_return FRG_CO_TRY(co_await mdio_->read(phyAddress_, bcm::shadow))
		& bcm::shadowDataMask;
}

async::result<nic::PhyResult<void>> Bcm54210EPhy::shadowWrite_(uint16_t reg, uint16_t value) {
	FRG_CO_TRY(co_await mdio_->write(phyAddress_, bcm::shadow,
					(reg << 10)
					| bcm::shadowWrEn
					| (value & bcm::shadowDataMask)));

	co_return {};
}


async::result<nic::PhyResult<void>> Bcm54210EPhy::configureClockDelays_() {
	bool wantRxcSkew = mode_ == nic::PhyMode::rgmiiId || mode_ == nic::PhyMode::rgmiiRxid;
	bool wantTxcSkew = mode_ == nic::PhyMode::rgmiiId || mode_ == nic::PhyMode::rgmiiTxid;

	std::println("Bcm54210EPhy: Configuring RGMII clock skew: RXC? {}, TXC? {}",
			wantRxcSkew, wantTxcSkew);

	// Configure RXC delay.

	auto val = FRG_CO_TRY(co_await auxctlRead_(bcm::auxctlShadowSelMisc));
	val |= bcm::auxctlShadowSelMiscWrEn;

	if (wantRxcSkew)
		val |= bcm::auxctlShadowSelMiscRxcSkewEn;
	else
		val &= ~bcm::auxctlShadowSelMiscRxcSkewEn;

	FRG_CO_TRY(co_await auxctlWrite_(bcm::auxctlShadowSelMisc, val));

	// Configure TXC delay.

	val = FRG_CO_TRY(co_await shadowRead_(bcm::shadowClkCtrl));

	if (wantTxcSkew)
		val |= bcm::shadowClkCtrlGTxClkEn;
	else
		val &= ~bcm::shadowClkCtrlGTxClkEn;

	FRG_CO_TRY(co_await shadowWrite_(bcm::shadowClkCtrl, val));

	co_return {};
}

async::result<nic::PhyResult<void>> Bcm54210EPhy::enableWirespeed_() {
	std::println("Bcm54210EPhy: Enabling Ethernet@Wirespeed");

	// Enable Ethernet@Wirespeed
	auto val = FRG_CO_TRY(co_await auxctlRead_(bcm::auxctlShadowSelMisc));
	val |= bcm::auxctlShadowSelMiscWrEn;
	val |= bcm::auxctlShadowSelMiscWirespeed;
	FRG_CO_TRY(co_await auxctlWrite_(bcm::auxctlShadowSelMisc, val));

	co_return {};
}

async::result<nic::PhyResult<void>> Bcm54210EPhy::configureLeds_() {
	std::println("Bcm54210EPhy: Configuring LEDs");

	FRG_CO_TRY(co_await shadowWrite_(bcm::shadowLeds1, bcm::shadowLeds1Led13Multicolor1));

	FRG_CO_TRY(co_await mdio_->write(phyAddress_, bcm::expSel, bcm::expMulticolor));
	FRG_CO_TRY(co_await mdio_->write(phyAddress_, bcm::expData,
					bcm::expMulticolorInPhase
					| bcm::expMulticolorLed13LinkAct));

	co_return {};
}

} // namespace netserver::phy
