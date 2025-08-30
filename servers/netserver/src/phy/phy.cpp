#include <print>

#include <netserver/phy.hpp>

#include "generic.hpp"
#include "realtek.hpp"

namespace nic {

using namespace netserver::phy;

async::result<std::shared_ptr<EthernetPhy>>
makeEthernetPhy(std::shared_ptr<Mdio> mdio, uint8_t phyAddress, PhyMode mode) {
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
			co_return std::make_shared<Rtl8211fPhy>(mdio, phyAddress, mode);
		default:
			std::println("phy: Unknown PHY ID: {:#x}, using generic PHY driver", physId);
			co_return std::make_shared<GenericEthernetPhy>(mdio, phyAddress, mode);
	}
}

} // namespace nic
