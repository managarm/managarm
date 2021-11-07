#pragma once

#include <thor-internal/pci/pci.hpp>
#include <thor-internal/dtb/dtb.hpp>
#include <arch/mem_space.hpp>

namespace thor::pci {

struct BrcmStbPcie final : PciConfigIo {
	BrcmStbPcie(DeviceTreeNode *node, uint16_t seg, uint8_t busStart, uint8_t busEnd);

	uint8_t readConfigByte(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset) override;
	uint16_t readConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset) override;
	uint32_t readConfigWord(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset) override;

	void writeConfigByte(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset, uint8_t value) override;
	void writeConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset, uint16_t value) override;
	void writeConfigWord(uint32_t seg, uint32_t bus, uint32_t slot,
			uint32_t function, uint16_t offset, uint32_t value) override;

private:
	void init_();
	void reset_();
	void enable_();

	void setOutboundWindow_(int n, uint64_t cpuAddr, uint64_t pcieAddr, size_t size);

	arch::mem_space configSpaceFor_(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function);

	uint32_t mdioRead_(uint8_t port, uint8_t reg);
	void mdioWrite_(uint8_t port, uint8_t reg, uint16_t val);
	void enableSSC_();

	uint16_t seg_;
	uint8_t busStart_;
	uint8_t busEnd_;

	arch::mem_space regSpace_;
};

} // namespace thor::pci
