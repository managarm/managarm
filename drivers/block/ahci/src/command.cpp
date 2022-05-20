#include <cstring>
#include <cstdio>
#include <inttypes.h>
#include <unistd.h>

#include <helix/memory.hpp>

#include "command.hpp"

Command::Command(uint64_t sector, size_t numSectors, size_t numBytes, void *buffer,
		CommandType type) : sector_{sector}, numSectors_{numSectors}, numBytes_{numBytes},
	buffer_{buffer}, type_{type}, event_{} {

	// TODO: Requests larger than 64k need to be split
	assert(numBytes < 65536);

	if (logCommands) {
		printf("block/ahci: queueing %zu byte %s to %p at sector %" PRIu64 "\n",
			numBytes, cmdTypeToString(type_),
			reinterpret_cast<void *>(buffer), sector);
	}
}

void Command::notifyCompletion() {
	if (logCommands) {
		printf("block/ahci: completed %s to %p\n", cmdTypeToString(type_), buffer_);
	}

	event_.raise();
}

void Command::prepare(commandTable& table, commandHeader& header) {
	auto tablePhys = helix::ptrToPhysical(&table);
	assert((tablePhys & 0x7F) == 0 && tablePhys < std::numeric_limits<uint32_t>::max());
	assert(numSectors_ < std::numeric_limits<uint16_t>::max());

	memset(&table, 0, sizeof(commandTable));
	table.commandFis.fisType = 0x27; // Host to Device FIS
	table.commandFis.info = 1 << 7; // Use command register, not control register
	table.commandFis.command = 0; // Set below
	table.commandFis.devHead = 1 << 6; // LBA bit
	table.commandFis.lba0 = sector_ & 0xFF;
	table.commandFis.lba1 = (sector_ >> 8) & 0xFF;
	table.commandFis.lba2 = (sector_ >> 16) & 0xFF;
	table.commandFis.lba3 = (sector_ >> 24) & 0xFF;
	table.commandFis.lba4 = (sector_ >> 32) & 0xFF;
	table.commandFis.lba5 = (sector_ >> 40) & 0xFF;
	table.commandFis.sectorCount = static_cast<uint16_t>(numSectors_);

	auto numEntries = writeScatterGather_(table);

	memset(&header, 0, sizeof(commandHeader));
	header.configBytes[0] = sizeof(fisH2D) / 4; // Supply length in dwords
	header.configBytes[1] = 0;
	header.prdtLength = numEntries;
	header.prdByteCount = 0;
	header.ctBase = static_cast<uint32_t>(helix::ptrToPhysical(&table));
	header.ctBaseUpper = 0;

	switch (type_) {
		case CommandType::read:
			table.commandFis.command = 0x25; // READ DMA EXT
			break;
		case CommandType::write:
			table.commandFis.command = 0x35; // WRITE DMA EXT
			header.configBytes[0] |= 1 << 6; // Indicates we are writing
			break;
		case CommandType::identify:
			table.commandFis.command = 0xEC; // IDENTIFY DEVICE
			break;
		default:
			assert(!"unknown command type");
	}

	if (logCommands) {
		printf("block/ahci: submitting %zu byte %s to %p at sector %" PRIu64 "\n",
				numBytes_, cmdTypeToString(type_), buffer_, sector_);
	}
}

/* Returns the number of PRDT entries written.
 *
 * Note on buffer_: libblockfs guarantees us that buffer_ is locked into memory,
 * and calling helPointerPhysical ensures that the pages are allocated and present
 * in the page tables. Hence, we know the buffer remains in memory during the DMA.
 */
size_t Command::writeScatterGather_(commandTable& table) {
	// TODO: Grab the page size for each individual address
	size_t pageSize = getpagesize();

	size_t prdtIndex = 0;
	auto addEntry = [&](uintptr_t phys, size_t bytesToWrite) {
		assert(prdtIndex < commandTable::prdtEntries &&
				phys < std::numeric_limits<uint32_t>::max() && !(phys & 1));

		table.prdts[prdtIndex++] = prdtEntry {
			static_cast<uint32_t>(phys),
			0,
			0,
			static_cast<uint32_t>(std::min(pageSize, bytesToWrite)) - 1,
		};
	};

	uintptr_t virtStart = reinterpret_cast<uintptr_t>(buffer_);
	uintptr_t virtEnd = virtStart + numBytes_;
	assert(virtEnd > virtStart);

	// As virtStart may not be aligned to pageSize, we split off the initial
	// unaligned part, then work with pageSize aligned chunks.
	if (virtStart % pageSize > 0) {
		auto nextAlignedAddr = (virtStart + pageSize) & ~(pageSize - 1);
		auto bytesUntilAligned = nextAlignedAddr - virtStart;
		auto bytesToWrite = std::min(numBytes_, bytesUntilAligned);
		addEntry(helix::addressToPhysical(virtStart), bytesToWrite);

		virtStart = nextAlignedAddr;
	}

	// Insert every page in the buffer into the scatter-gather list.
	for (uintptr_t virt = virtStart; virt < virtEnd; virt += pageSize) {
		uintptr_t phys = helix::addressToPhysical(virt);
		
		// TODO: As a small optimisation, we could accumulate into the previous entry if they
		// happen to be physically contiguous.
		addEntry(phys, virtEnd - virt);
	}

	return prdtIndex;
}
