#pragma once

#include <cstddef>
#include <cstdint>

namespace limits {
	constexpr size_t MAX_CMD_SLOTS = 32;
	constexpr size_t MAX_PORTS	   = 32;
}

struct received_fis {
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
static_assert(sizeof(received_fis) == 256);

struct command_header {
	uint8_t configBytes[2];
	uint16_t prdtLength;
	uint32_t prdByteCount;
	uint32_t ctBase;
	uint32_t ctBaseUpper;
	uint32_t _reserved[4];
};
static_assert(sizeof(command_header) == 32);

struct command_list {
	command_header slots[32];
};
static_assert(sizeof(command_list) == 32 * 32);

struct prdt_entry {
	uint32_t dataBase;
	uint32_t dataBaseUpper;
	uint32_t _reserved;
	uint32_t info;
};
static_assert(sizeof(prdt_entry) == 16);

struct fis_h2d {
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
static_assert(sizeof(fis_h2d) == 20);

struct command_table {
	/* uint8_t commandFis[0x40]; */
	fis_h2d commandFis;
	uint8_t commandFisPad[0x40 - 20];

	uint8_t atapiCommand[0x10];
	uint8_t _reserved[0x30];

	// Allows us to read 64kb into a buffer (16 * 512), plus one to deal with unaligned buffers.
	static constexpr std::size_t PRDT_ENTRIES = 16 + 1;
	prdt_entry prdts[PRDT_ENTRIES];
};
static_assert(sizeof(command_table) == 128 + 16 * command_table::PRDT_ENTRIES);

struct identify_device {
	uint16_t _junkA[27];
	uint16_t model[20];
	uint16_t _junkB[36];
	uint16_t capabilities;
	uint16_t _junkC[16];
	uint64_t maxLBA48;
	uint16_t _junkD[152];

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

	bool supportsLba48() const {
		return capabilities & (1 << 10);
	}
};
static_assert(sizeof(identify_device) == 512);
