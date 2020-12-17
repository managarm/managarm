#pragma once

#include <thor-internal/pci/pci.hpp>

namespace thor::pci {

[[gnu::weak]] uint32_t readLegacyPciConfigWord(uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset);
[[gnu::weak]] uint16_t readLegacyPciConfigHalf(uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset);
[[gnu::weak]] uint8_t readLegacyPciConfigByte(uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset);

[[gnu::weak]] void writeLegacyPciConfigWord(uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset, uint32_t value);
[[gnu::weak]] void writeLegacyPciConfigHalf(uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset, uint16_t value);
[[gnu::weak]] void writeLegacyPciConfigByte(uint32_t bus, uint32_t slot,
		uint32_t function, uint32_t offset, uint8_t value);


struct LegacyPciConfigIo : PciConfigIo {
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
};

} // namespace thor::pci
