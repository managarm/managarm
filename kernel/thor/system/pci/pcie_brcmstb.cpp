#include <thor-internal/pci/pcie_brcmstb.hpp>
#include <thor-internal/timer.hpp>
#include <thor-internal/fiber.hpp>
#include <arch/mem_space.hpp>

namespace thor::pci {

namespace reg {
	arch::bit_register<uint16_t> lnksta{0x00be};
	arch::scalar_register<uint32_t> hwRev{0x406c};
	arch::bit_register<uint32_t> bridgeCtl{0x9210};
	arch::bit_register<uint32_t> bridgeState{0x4068};
	arch::bit_register<uint32_t> hardDebug{0x4204};
	arch::bit_register<uint32_t> miscCtl{0x4008};

	arch::scalar_register<uint32_t> rcBar1Lo{0x402c};
	arch::scalar_register<uint32_t> rcBar2Lo{0x4034};
	arch::scalar_register<uint32_t> rcBar2Hi{0x4038};
	arch::scalar_register<uint32_t> rcBar3Lo{0x403c};

	arch::bit_register<uint32_t> vendorReg1{0x0188};
	arch::bit_register<uint32_t> priv1IdVal3{0x043c};
	arch::bit_register<uint32_t> priv1LinkCap{0x04dc};

	arch::bit_register<uint32_t> cfgIndex{0x9000};

	inline constexpr uint64_t cfgData = 0x8000;

	arch::bit_register<uint32_t> mdioAddr{0x1100};
	arch::bit_register<uint32_t> mdioWrData{0x1104};
	arch::bit_register<uint32_t> mdioRdData{0x1108};
} // namespace reg

namespace lnksta {
	arch::field<uint16_t, uint8_t> linkSpeed{0, 4};
	arch::field<uint16_t, uint8_t> negotiatedLinkWidth{4, 6};

	constexpr const char *linkSpeedString(uint8_t v) {
		switch (v) {
			case 0: return "down";
			case 1: return "2.5 GT/s";
			case 2: return "5.0 GT/s";
			case 4: return "8.0 GT/s";
			default: return "unknown";
		}
	}
} // namespace lnksta

namespace bridgeCtl {
	arch::field<uint32_t, bool> reset{0, 1};
	arch::field<uint32_t, bool> swInit{1, 1};
} // namespace bridgeCtl

namespace bridgeState {
	arch::field<uint32_t, bool> rcMode{7, 1};
	arch::field<uint32_t, bool> dlActive{5, 1};
	arch::field<uint32_t, bool> phyActive{4, 1};
} // namespace bridgeState

namespace hardDebug {
	arch::field<uint32_t, bool> serdesDisable{27, 1};
	arch::field<uint32_t, bool> clkreqEnable{1, 1};
} // namespace hardDebug

namespace miscCtl {
	arch::field<uint32_t, bool> accessEnable{12, 1};
	arch::field<uint32_t, bool> readUrMode{13, 1};
	arch::field<uint32_t, uint8_t> maxBurstSize{20, 2};
	arch::field<uint32_t, uint8_t> scbSize0{27, 5};
} // namespace miscCtl

namespace rcBar {
	inline constexpr uint32_t sizeMask = 0x1f;

	uint32_t encodeSize(size_t size) {
		auto n = 63 - __builtin_clzll(size);

		if (n >= 12 && n <= 15)
			return (n - 12) + 0x1c;
		else if (n >= 16 && n <= 35)
			return n - 15;
		return 0;
	}
} // namespace rcBar

namespace vendorReg1 {
	arch::field<uint32_t, uint8_t> endianMode{2, 2};
} // namespace vendorReg1

namespace priv1 {
	arch::field<uint32_t, uint32_t> id{0, 24};
	arch::field<uint32_t, uint8_t> linkCap{10, 2};
} // namespace priv1

namespace cfgIndex {
	arch::field<uint32_t, uint8_t> bus{20, 8};
	arch::field<uint32_t, uint8_t> slot{15, 5};
	arch::field<uint32_t, uint8_t> function{12, 3};
} // namespace cfgIndex

namespace mdio {
	arch::field<uint32_t, uint16_t> pktCmd{20, 12};
	arch::field<uint32_t, uint8_t> pktPort{16, 4};
	arch::field<uint32_t, uint16_t> pktReg{0, 16};

	arch::field<uint32_t, uint32_t> data{0, 31};
	arch::field<uint32_t, bool> dataDone{31, 1};
} // namespace mdio

BrcmStbPcie::BrcmStbPcie(DeviceTreeNode *node, uint16_t seg, uint8_t busStart, uint8_t busEnd)
: seg_{seg}, busStart_{busStart}, busEnd_{busEnd} {
	auto addr = node->reg()[0].addr;
	auto size = (node->reg()[0].size + 0xFFF) & ~0xFFF;

	auto ptr = KernelVirtualMemory::global().allocate(size);
	for (size_t i = 0; i < size; i += 0x1000) {
		KernelPageSpace::global().mapSingle4k(
				VirtualAddr(ptr) + i, addr + i,
				page_access::write, CachingMode::mmioNonPosted);
	}

	regSpace_ = arch::mem_space{ptr};

	init_();
}

void BrcmStbPcie::init_() {
	reset_();

	auto rev = regSpace_.load(reg::hwRev) & 0xFFFF;
	infoLogger() << "thor: BrcmStb revision: " << frg::hex_fmt{rev} << frg::endlog;

	// Configure windows

	regSpace_.store(reg::miscCtl, regSpace_.load(reg::miscCtl)
			/ miscCtl::accessEnable(true)
			/ miscCtl::readUrMode(true)
			/ miscCtl::maxBurstSize(/* 128 bytes */ 0));

	// TODO: read this out of the DT

	regSpace_.store(reg::rcBar2Lo, 0 | rcBar::encodeSize(0x200000000));
	regSpace_.store(reg::rcBar2Hi, 0);

	regSpace_.store(reg::miscCtl, regSpace_.load(reg::miscCtl)
			/ miscCtl::scbSize0(63 - __builtin_clzll(0x200000000) - 15));

	regSpace_.store(reg::rcBar1Lo, regSpace_.load(reg::rcBar1Lo) & ~rcBar::sizeMask);
	regSpace_.store(reg::rcBar3Lo, regSpace_.load(reg::rcBar3Lo) & ~rcBar::sizeMask);

	enable_();

	for (int i = 0; ; i++) {
		auto state = regSpace_.load(reg::bridgeState);
		if (state & bridgeState::dlActive && state & bridgeState::phyActive)
			break;

		KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(5'000'000));

		if (i >= 100) {
			panicLogger() << "thor: Bridge failed to start" << frg::endlog;
		}
	}

	if (!(regSpace_.load(reg::bridgeState) & bridgeState::rcMode))
		panicLogger() << "thor: Bridge is in EP mode" << frg::endlog;

	// TODO: read this out of the DT
	setOutboundWindow_(0, 0x600000000, 0xC0000000, 0x40000000);

	regSpace_.store(reg::priv1LinkCap, regSpace_.load(reg::priv1LinkCap)
			/ priv1::linkCap(0b11)); // L1 & L0s

	regSpace_.store(reg::priv1IdVal3, regSpace_.load(reg::priv1IdVal3)
			/ priv1::id(0x060400));

	enableSSC_();

	auto ls = regSpace_.load(reg::lnksta);
	infoLogger() << "thor: Link is up, speed "
		<< lnksta::linkSpeedString(ls & lnksta::linkSpeed)
		<< ", x" << (ls & lnksta::negotiatedLinkWidth)
		<< frg::endlog;

	regSpace_.store(reg::vendorReg1, regSpace_.load(reg::vendorReg1)
			/ vendorReg1::endianMode(0));

	regSpace_.store(reg::hardDebug, regSpace_.load(reg::hardDebug)
			/ hardDebug::clkreqEnable(true));
}

void BrcmStbPcie::reset_() {
	regSpace_.store(reg::bridgeCtl, regSpace_.load(reg::bridgeCtl)
			/ bridgeCtl::swInit(true));

	KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(200'000));

	regSpace_.store(reg::bridgeCtl, regSpace_.load(reg::bridgeCtl)
			/ bridgeCtl::swInit(false));

	KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(200'000));

	regSpace_.store(reg::hardDebug, regSpace_.load(reg::hardDebug)
			/ hardDebug::serdesDisable(false));

	KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(100'000));
}

void BrcmStbPcie::enable_() {
	regSpace_.store(reg::bridgeCtl, regSpace_.load(reg::bridgeCtl)
			/ bridgeCtl::reset(false));

	KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(100'000));
}

void BrcmStbPcie::setOutboundWindow_(int n, uint64_t cpuAddr, uint64_t pcieAddr, size_t size) {
	arch::scalar_register<uint32_t> pcieLo{0x400c + n * 8};
	arch::scalar_register<uint32_t> pcieHi{0x4010 + n * 8};

	regSpace_.store(pcieLo, pcieAddr);
	regSpace_.store(pcieHi, pcieAddr >> 32);

	arch::bit_register<uint32_t> baseLimit{0x4070 + n * 4};
	arch::field<uint32_t, uint16_t> base{4, 12};
	arch::field<uint32_t, uint16_t> limit{20, 12};

	auto baseMB = cpuAddr / 0x100000;
	auto limitMB = (cpuAddr + size - 1) / 0x100000;

	regSpace_.store(baseLimit, regSpace_.load(baseLimit)
			/ base(cpuAddr / 0x100000)
			/ limit(limitMB));

	constexpr uint64_t hiShift = 12;

	arch::bit_register<uint32_t> baseHi{0x4080 + n * 8};
	arch::bit_register<uint32_t> limitHi{0x4084 + n * 8};
	arch::field<uint32_t, uint8_t> hiMask{0, 8};

	regSpace_.store(baseHi, regSpace_.load(baseHi)
		/ hiMask(baseMB >> hiShift));
	regSpace_.store(limitHi, regSpace_.load(limitHi)
		/ hiMask(limitMB >> hiShift));
}

uint32_t BrcmStbPcie::mdioRead_(uint8_t port, uint8_t reg) {
	regSpace_.store(reg::mdioAddr, mdio::pktPort(port)
			| mdio::pktReg(reg)
			| mdio::pktCmd(1));

	regSpace_.load(reg::mdioAddr);

	int tries = 0;
	while (true) {
		auto data = regSpace_.load(reg::mdioRdData);
		if (data & mdio::dataDone) {
			return data & mdio::data;
		}

		KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(10'000'000));
		if (tries++ > 10) {
			panicLogger() << "thor: MDIO read failure" << frg::endlog;
		}
	}

	__builtin_unreachable();
}

void BrcmStbPcie::mdioWrite_(uint8_t port, uint8_t reg, uint16_t val) {
	regSpace_.store(reg::mdioAddr, mdio::pktPort(port)
			| mdio::pktReg(reg)
			| mdio::pktCmd(0));

	regSpace_.load(reg::mdioAddr);

	regSpace_.store(reg::mdioWrData, mdio::dataDone(1)
			| mdio::data(val));

	int tries = 0;
	while (true) {
		auto data = regSpace_.load(reg::mdioWrData);
		if (!(data & mdio::dataDone)) {
			break;
		}

		KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(10'000'000));
		if (tries++ > 10) {
			panicLogger() << "thor: MDIO write failure" << frg::endlog;
		}
	}
}

void BrcmStbPcie::enableSSC_() {
	mdioWrite_(0, 0x1f, 0x1100);
	auto ctl = mdioRead_(0, 0x0002);
	ctl |= 0x8000;
	ctl |= 0x4000;
	mdioWrite_(0, 0x0002, ctl);

	KernelFiber::asyncBlockCurrent(generalTimerEngine()->sleepFor(2'000'000));

	auto status = mdioRead_(0, 0x0001);

	assert((status & 0x400) && (status & 0x800));
}

arch::mem_space BrcmStbPcie::configSpaceFor_(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function) {
	assert(seg == seg_);
	assert(bus >= busStart_);
	assert(bus <= busEnd_);

	// Bus 0 accesses controller MMIO
	if (bus == busStart_) {
		assert(!slot && !function);
		return regSpace_;
	}

	regSpace_.store(reg::cfgIndex, cfgIndex::bus(bus)
			| cfgIndex::slot(slot)
			| cfgIndex::function(function));
	return regSpace_.subspace(reg::cfgData);
}

uint8_t BrcmStbPcie::readConfigByte(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint16_t offset) {
	if (bus == busStart_ && (slot || function))
		return 0xFF;

	auto space = configSpaceFor_(seg, bus, slot, function);
	return arch::scalar_load<uint8_t>(space, offset);
}

uint16_t BrcmStbPcie::readConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint16_t offset) {
	if (bus == busStart_ && (slot || function))
		return 0xFFFF;

	auto space = configSpaceFor_(seg, bus, slot, function);
	return arch::scalar_load<uint16_t>(space, offset);
}

uint32_t BrcmStbPcie::readConfigWord(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint16_t offset) {
	if (bus == busStart_ && (slot || function))
		return 0xFFFFFFFF;

	auto space = configSpaceFor_(seg, bus, slot, function);
	return arch::scalar_load<uint32_t>(space, offset);
}

void BrcmStbPcie::writeConfigByte(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint16_t offset, uint8_t value) {
	if (bus == busStart_ && (slot || function))
		return;

	auto space = configSpaceFor_(seg, bus, slot, function);
	arch::scalar_store<uint8_t>(space, offset, value);
}
void BrcmStbPcie::writeConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint16_t offset, uint16_t value) {
	if (bus == busStart_ && (slot || function))
		return;

	auto space = configSpaceFor_(seg, bus, slot, function);
	arch::scalar_store<uint16_t>(space, offset, value);
}
void BrcmStbPcie::writeConfigWord(uint32_t seg, uint32_t bus, uint32_t slot,
		uint32_t function, uint16_t offset, uint32_t value) {
	if (bus == busStart_ && (slot || function))
		return;

	auto space = configSpaceFor_(seg, bus, slot, function);
	arch::scalar_store<uint32_t>(space, offset, value);
}

} // namespace thor::pci
