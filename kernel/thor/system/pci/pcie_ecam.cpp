#include <thor-internal/pci/pci.hpp>
#include <thor-internal/pci/pcie_ecam.hpp>
#include <arch/register.hpp>

namespace thor::pci {

EcamPcieConfigIo::EcamPcieConfigIo(uintptr_t mmioBase, uint16_t seg,
		uint8_t busStart, uint8_t busEnd)
: space_{}, seg_{seg}, busStart_{busStart}, busEnd_{busEnd} {
	uintptr_t size = (busEnd - busStart + 1) << 20;

	auto register_ptr = KernelVirtualMemory::global().allocate(size);

	for (size_t i = 0; i < size; i += 0x1000) {
		KernelPageSpace::global().mapSingle4k(
				VirtualAddr(register_ptr) + i, mmioBase + i,
				page_access::write, CachingMode::uncached);
	}

	space_ = arch::mem_space{register_ptr};
}

uintptr_t EcamPcieConfigIo::calculateOffset(uint32_t bus, uint32_t slot,
		uint32_t function, uint16_t offset) {
	assert(offset < 0x1000);
	assert(bus >= busStart_ && bus <= busEnd_);
	return ((bus - busStart_) << 20) | (slot << 15) | (function << 12) | offset;
}

uint8_t EcamPcieConfigIo::readConfigByte(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset) {
	assert(seg == seg_);
	return arch::scalar_load<uint8_t>(space_, calculateOffset(bus, slot, function, offset));
}

uint16_t EcamPcieConfigIo::readConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset) {
	assert(seg == seg_);
	return arch::scalar_load<uint16_t>(space_, calculateOffset(bus, slot, function, offset));
}

uint32_t EcamPcieConfigIo::readConfigWord(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset) {
	assert(seg == seg_);
	return arch::scalar_load<uint32_t>(space_, calculateOffset(bus, slot, function, offset));
}

void EcamPcieConfigIo::writeConfigByte(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset, uint8_t value) {
	assert(seg == seg_);
	return arch::scalar_store(space_, calculateOffset(bus, slot, function, offset), value);
}

void EcamPcieConfigIo::writeConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset, uint16_t value) {
	assert(seg == seg_);
	return arch::scalar_store(space_, calculateOffset(bus, slot, function, offset), value);
}

void EcamPcieConfigIo::writeConfigWord(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset, uint32_t value) {
	assert(seg == seg_);
	return arch::scalar_store(space_, calculateOffset(bus, slot, function, offset), value);
}

} // namespace thor::pci
