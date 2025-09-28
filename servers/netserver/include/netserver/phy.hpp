#pragma once

#include <async/result.hpp>
#include <cstdint>
#include <expected>

namespace nic {

enum class PhyError {
	none,
	timeout,
	hardwareError,
};

template <typename T>
using PhyResult = std::expected<T, PhyError>;

struct Mdio {
	virtual ~Mdio() = default;

	virtual async::result<PhyResult<uint16_t>> read(uint8_t phyAddress, uint8_t registerNum) = 0;

	virtual async::result<PhyResult<void>>
	write(uint8_t phyAddress, uint8_t registerNum, uint16_t value) = 0;
};

enum struct LinkSpeed {
	unknown,
	speed10,
	speed100,
	speed1000,
	speed2500,
	speed5000,
	speed10000,
};

enum struct LinkDuplex { unknown, half, full };

enum class PhyMode {
	rgmii,
	rgmiiRxid,
	rgmiiTxid,
	rgmiiId,
};

struct EthernetPhy {
	EthernetPhy(std::shared_ptr<Mdio> mdio, uint8_t phyAddress, PhyMode mode)
	: mdio_(std::move(mdio)),
	  phyAddress_(phyAddress),
	  mode_{mode} {}

	virtual ~EthernetPhy() = default;

	virtual async::result<PhyResult<void>> configure() = 0;
	virtual async::result<PhyResult<void>> startup() = 0;

	bool autoNegotiate() const { return autoNegotiate_; }
	bool linkStatus() const { return linkStatus_; }

	LinkSpeed speed() const { return speed_; }
	LinkDuplex duplex() const { return duplex_; }

	PhyMode mode() const { return mode_; }

protected:
	std::shared_ptr<Mdio> mdio_;
	uint8_t phyAddress_;

	bool autoNegotiate_ = true;
	bool linkStatus_ = false;

	LinkSpeed speed_ = LinkSpeed::unknown;
	LinkDuplex duplex_ = LinkDuplex::unknown;

	PhyMode mode_;
};

async::result<std::shared_ptr<EthernetPhy>>
makeEthernetPhy(std::shared_ptr<Mdio> mdio, uint8_t phyAddress, PhyMode mode);

} // namespace nic
