#pragma once

#include <thor-internal/pci/pci.hpp>
#include <arch/mem_space.hpp>
#include <frg/hash_map.hpp>

namespace thor::pci {

struct EcamPcieConfigIo final : PciConfigIo {
	EcamPcieConfigIo(uintptr_t mmioBase, uint16_t seg, uint8_t busStart, uint8_t busEnd);

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
	arch::mem_space spaceForBus_(uint32_t bus);
	uintptr_t calculateOffset_(uint32_t slot, uint32_t function,
			uint16_t offset);

	uintptr_t mmioBase_;

	frg::hash_map<
		uint32_t,
		void *,
		frg::hash<uint32_t>,
		KernelAlloc
	> busMappings_;

	uint16_t seg_;
	uint8_t busStart_;
	uint8_t busEnd_;
};

} // namespace thor::pci
