#include <thor-internal/pci/pci.hpp>
#include <thor-internal/pci/pci_legacy.hpp>

namespace thor::pci {

uint8_t LegacyPciConfigIo::readConfigByte(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset) {
	assert(!seg);
	return readLegacyPciConfigByte(bus, slot, function, offset);
}

uint16_t LegacyPciConfigIo::readConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset) {
	assert(!seg);
	return readLegacyPciConfigHalf(bus, slot, function, offset);
}

uint32_t LegacyPciConfigIo::readConfigWord(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset) {
	assert(!seg);
	return readLegacyPciConfigWord(bus, slot, function, offset);
}

void LegacyPciConfigIo::writeConfigByte(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset, uint8_t value) {
	assert(!seg);
	writeLegacyPciConfigByte(bus, slot, function, offset, value);
}

void LegacyPciConfigIo::writeConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset, uint16_t value) {
	assert(!seg);
	writeLegacyPciConfigHalf(bus, slot, function, offset, value);
}

void LegacyPciConfigIo::writeConfigWord(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset, uint32_t value) {
	assert(!seg);
	writeLegacyPciConfigWord(bus, slot, function, offset, value);
}

} // namespace thor::pci
