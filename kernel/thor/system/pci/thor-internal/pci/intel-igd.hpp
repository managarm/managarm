#pragma once

#include <cstdint>

namespace thor::pci {

#define IGD_OPREGION_SIGNATURE "IntelGraphicsMem"
#define IGD_OPREGION_SIZE (8 * 1024)

#define IGD_OPREGION_ASLE_OFFSET 0x300
#define IGD_OPREGION_VBT_OFFSET 0x400
#define IGD_OPREGION_ASLE_EXT_OFFSET 0x1C00

#define IGD_MBOX_ASLE (1 << 2)
#define IGD_MBOX_VBT (1 << 3)
#define IGD_MBOX_ASLE_EXT (1 << 4)

struct IgdOpregionHeader {
	char signature[16];
	uint32_t size;
	struct {
		uint8_t reserved;
		uint8_t revision;
		uint8_t minor;
		uint8_t major;
	} __attribute__((packed)) over;
	char sver[32];
	char vver[16];
	char gver[16];
	uint32_t mbox;
	uint32_t dmod;
	uint32_t pcon;
	char dver[32];
	uint8_t reserved[124];
} __attribute__((packed));

_Static_assert(sizeof(struct IgdOpregionHeader) == 256, "OpRegion header size is incorrect");

struct IgdOpregionAsle {
	uint32_t ardy;
	uint32_t aslc;
	uint32_t tche;
	uint32_t alsi;
	uint32_t bclp;
	uint32_t pfit;
	uint32_t cblv;
	uint16_t bclm[20];
	uint32_t cpfm;
	uint32_t epfm;
	uint8_t plut[74];
	uint32_t pfmb;
	uint32_t cddv;
	uint32_t pcft;
	uint32_t srot;
	uint32_t iuer;
	uint64_t fdss;
	uint32_t fdsp;
	uint32_t stat;
	uint64_t rvda;
	uint32_t rvds;
	uint8_t reserved[58];
} __attribute__((packed));

} // namespace thor::pci
