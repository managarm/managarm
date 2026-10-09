#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <string>

namespace limits {
	constexpr size_t maxCmdSlots = 32;
	constexpr size_t maxPorts    = 32;
}

namespace {
	constexpr bool logCommands = false;
}

struct receivedFis {
	uint8_t dmaFis[0x1C];
	uint8_t _reservedA[4];
	uint8_t pioFis[0x14];
	uint8_t _reservedB[12];
	uint8_t d2hFis[0x14];
	uint8_t _reservedC[4];
	uint8_t sdbFis[8];
	uint8_t unkFis[0x40];
	uint8_t _reservedD[0x60];
};
static_assert(sizeof(receivedFis) == 256);

struct commandHeader {
	uint8_t configBytes[2];
	uint16_t prdtLength;
	uint32_t prdByteCount;
	uint32_t ctBase;
	uint32_t ctBaseUpper;
	uint32_t _reserved[4];
};
static_assert(sizeof(commandHeader) == 32);

struct commandList {
	commandHeader slots[32];
};
static_assert(sizeof(commandList) == 32 * 32);

struct prdtEntry {
	uint32_t dataBase;
	uint32_t dataBaseUpper;
	uint32_t _reserved;
	uint32_t info;
};
static_assert(sizeof(prdtEntry) == 16);

struct fisH2D {
	uint8_t fisType;
	uint8_t info;
	uint8_t command;
	uint8_t features;

	uint8_t lba0;
	uint8_t lba1;
	uint8_t lba2;
	uint8_t devHead;

	uint8_t lba3;
	uint8_t lba4;
	uint8_t lba5;
	uint8_t featuresUpper;

	uint16_t sectorCount;
	uint8_t _reservedA;
	uint8_t control;

	uint32_t _reservedB;
};
static_assert(sizeof(fisH2D) == 20);

struct alignas(128) commandTable {
	fisH2D commandFis;
	uint8_t commandFisPad[0x40 - 20];

	uint8_t atapiCommand[0x10];
	uint8_t _reserved[0x30];

	// Allows us to read 64kb into a buffer (16 * 512), plus one to deal with unaligned buffers.
	static constexpr std::size_t prdtEntries = 16 + 1;
	prdtEntry prdts[prdtEntries];
};
static_assert(alignof(commandTable) >= 128);

struct identifyDevice {
	uint16_t _junkA[27];
	uint16_t model[20];
	uint16_t _junkB[35];
	uint16_t commandSetSupport1;
	uint16_t commandSetSupport2;
	uint16_t _reserved84;
	uint16_t commandSetEnabled1;
	uint16_t _reserved86;
	uint16_t commandSetEnabledValidity;
	uint16_t _junkC[12];
	uint64_t maxLBA48;
	uint16_t _junkD[2];
	uint16_t sectorSizeInfo;
	uint16_t _junkE[9];
	uint16_t logicalSectorSize;
	uint16_t _junkF[139];

	std::string getModel() const {
		char modelNative[41];
		memcpy(modelNative, model, 40);
		modelNative[40] = 0;
		
		// Model name is returned as big endian, swap each two byte pair to fix that
		for (int i = 0; i < 40; i += 2) {
			std::swap(modelNative[i], modelNative[i + 1]); 
		}

		std::string out{modelNative};

		// Chop off the spaces at the end
		auto cutPos = out.find_last_not_of(' ');
		if (cutPos != std::string::npos) {
			out.resize(cutPos + 1);
		}

		return out;
	}

	// Returns logical and physical sector sizes
	std::pair<size_t, size_t> getSectorSize() const {
		if (sectorSizeInfo & (1 << 14) && !(sectorSizeInfo & (1 << 15))) {
			auto logical = 512;
			if (sectorSizeInfo & (1 << 12)) {
				// Logical sector size is greater than 512 bytes
				logical = logicalSectorSize;
				assert(logical > 512);
			}

			auto physical = (1 << (sectorSizeInfo & 0xF)) * logical;
			assert(physical <= 4096);
			return { logical, physical };
		} else {
			// Word is invalid, just assume 512 / 512
			return { 512, 512 };
		}
	}

	bool supportsLba48() const {
		return commandSetSupport2 & (1 << 10);
	}

	bool writeCacheKnownDisabled() const {
		// Words 82/83 report support; words 85/87 report enabled features.
		return ((commandSetSupport2 & 0xC000) == 0x4000 && !(commandSetSupport1 & (1 << 5)))
				|| ((commandSetEnabledValidity & 0xC000) == 0x4000 && !(commandSetEnabled1 & (1 << 5)));
	}

	bool supportsFlush() const {
		return (commandSetSupport2 & 0xC000) == 0x4000 && (commandSetSupport2 & (1 << 12));
	}

	bool supportsFlushExt() const {
		return (commandSetSupport2 & 0xC000) == 0x4000 && (commandSetSupport2 & (1 << 13));
	}
};
static_assert(sizeof(identifyDevice) == 512);
