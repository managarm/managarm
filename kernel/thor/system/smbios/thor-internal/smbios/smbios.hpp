#pragma once

#include <stdint.h>

namespace thor::smbios {

struct Smbios3Header {
	char anchor[5];
	uint8_t checksum;
	uint8_t length;
	uint8_t majorVersion;
	uint8_t minorVersion;
	uint8_t docRev;
	uint8_t revision;
	uint8_t reserved;
	uint32_t maxTableSize;
	uint64_t tableAddress;
};
static_assert(sizeof(Smbios3Header) == 0x18);

void publish();

} // namespace thor::smbios
