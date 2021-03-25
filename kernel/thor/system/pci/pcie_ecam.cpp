#include <thor-internal/pci/pci.hpp>
#include <thor-internal/pci/pcie_ecam.hpp>
#include <arch/register.hpp>

namespace thor::pci {

EcamPcieConfigIo::EcamPcieConfigIo(uintptr_t mmioBase, uint16_t seg,
		uint8_t busStart, uint8_t busEnd)
: mmioBase_{mmioBase}, busMappings_{frg::hash<uint32_t>{}, *kernelAlloc},
		seg_{seg}, busStart_{busStart}, busEnd_{busEnd} { }

arch::mem_space EcamPcieConfigIo::spaceForBus_(uint32_t bus) {
	assert(bus >= busStart_ && bus <= busEnd_);

	if (auto mapping = busMappings_.get(bus); mapping) {
		return arch::mem_space{*mapping};
	}

	constexpr uintptr_t size = 1 << 20;
	uintptr_t offset = uintptr_t(bus - busStart_) << 20;

	auto ptr = KernelVirtualMemory::global().allocate(size);

	for (size_t i = 0; i < size; i += 0x1000) {
		KernelPageSpace::global().mapSingle4k(
				VirtualAddr(ptr) + i, mmioBase_ + i + offset,
				page_access::write, CachingMode::uncached);
	}

	busMappings_.insert(bus, ptr);

	return arch::mem_space{ptr};
}

uintptr_t EcamPcieConfigIo::calculateOffset_(uint32_t slot, uint32_t function,
		uint16_t offset) {
	assert(offset < 0x1000);
	return (slot << 15) | (function << 12) | offset;
}

uint8_t EcamPcieConfigIo::readConfigByte(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset) {
	assert(seg == seg_);
	auto space = spaceForBus_(bus);
	auto spaceOffset = calculateOffset_(slot, function, offset);
	return arch::scalar_load<uint8_t>(space, spaceOffset);
}

uint16_t EcamPcieConfigIo::readConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset) {
	assert(seg == seg_);
	auto space = spaceForBus_(bus);
	auto spaceOffset = calculateOffset_(slot, function, offset);
	return arch::scalar_load<uint16_t>(space, spaceOffset);
}

uint32_t EcamPcieConfigIo::readConfigWord(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset) {
	assert(seg == seg_);
	auto space = spaceForBus_(bus);
	auto spaceOffset = calculateOffset_(slot, function, offset);
	return arch::scalar_load<uint32_t>(space, spaceOffset);
}

void EcamPcieConfigIo::writeConfigByte(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset, uint8_t value) {
	assert(seg == seg_);
	auto space = spaceForBus_(bus);
	auto spaceOffset = calculateOffset_(slot, function, offset);
	arch::scalar_store<uint8_t>(space, spaceOffset, value);
}

void EcamPcieConfigIo::writeConfigHalf(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset, uint16_t value) {
	assert(seg == seg_);
	auto space = spaceForBus_(bus);
	auto spaceOffset = calculateOffset_(slot, function, offset);
	arch::scalar_store<uint16_t>(space, spaceOffset, value);
}

void EcamPcieConfigIo::writeConfigWord(uint32_t seg, uint32_t bus, uint32_t slot, uint32_t function, uint16_t offset, uint32_t value) {
	assert(seg == seg_);
	auto space = spaceForBus_(bus);
	auto spaceOffset = calculateOffset_(slot, function, offset);
	arch::scalar_store<uint32_t>(space, spaceOffset, value);
}

} // namespace thor::pci
