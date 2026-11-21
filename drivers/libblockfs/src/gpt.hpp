#pragma once

#include <arch/endian.hpp>
#include <string.h>
#include <memory>
#include <string>
#include <vector>
#include <utility>

#include <blockfs.hpp>

namespace blockfs {
namespace gpt {

struct Guid {
	bool operator== (const Guid &other) {
		return a == other.a && b == other.b && c == other.c
				&& !memcmp(d, other.d, 2)
				&& !memcmp(e, other.e, 6);
	}

	bool operator!= (const Guid &other) {
		return !(*this == other);
	}

	// Canonical lowercase form, as printed by Linux and blkid.
	std::string toString() const;

	arch::le_u32 a;
	arch::le_u16 b;
	arch::le_u16 c;
	uint8_t d[2];
	uint8_t e[6];
};
static_assert(sizeof(Guid) == 16, "Bad sizeof(Guid)");

namespace type_guids {
	static constexpr Guid null{0, 0, 0, {0, 0}, {0, 0, 0, 0, 0, 0}};
	static constexpr Guid windowsData{0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0},
			{0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};
	static constexpr Guid managarmRootPartition{0x64212B3B, 0x56A1, 0x4DFB, {0x97, 0x1E},
			{0xBC, 0x8C, 0xD0, 0x27, 0x99, 0x6A}};
	static constexpr Guid managarmSwapPartition{0xD9D27EE8, 0xD26F, 0x4696, {0xB1, 0xB2},
			{0x9A, 0x28, 0x10, 0x23, 0x5D, 0xE5}};
};

// --------------------------------------------------------
// On-disk structures
// --------------------------------------------------------

struct DiskHeader {
	arch::le_u64 signature;
	arch::le_u32 revision;
	arch::le_u32 headerSize;
	arch::le_u32 headerCheckSum;
	arch::le_u32 reservedZero;
	arch::le_u64 currentLba;
	arch::le_u64 backupLba;
	arch::le_u64 firstLba;
	arch::le_u64 lastLba;
	uint8_t diskGuid[16];
	arch::le_u64 entryTableLba;
	arch::le_u32 numEntries;
	arch::le_u32 entrySize;
	arch::le_u32 tableCheckSum;
	uint8_t padding[420];
};
static_assert(sizeof(DiskHeader) == 512, "Bad GPT header struct size");

struct DiskEntry {
	Guid typeGuid;
	Guid uniqueGuid;
	arch::le_u64 firstLba;
	arch::le_u64 lastLba;
	arch::le_u64 attrFlags;
	uint8_t partitionName[72];
};
static_assert(sizeof(DiskEntry) == 128, "Bad GPT entry struct size");

// --------------------------------------------------------
// Table
// --------------------------------------------------------

struct Partition;

struct Table {
public:
	Table(BlockDevice *device);

	BlockDevice *getDevice();

	async::result<std::expected<void, Error>> parse();

	size_t numPartitions();

	Partition &getPartition(int index);

private:
	async::result<std::expected<std::pair<arch::dma_buffer, DiskHeader *>, Error>>
	probeSectorSize_(size_t sectorSize);

	BlockDevice *device_;
	std::vector<std::unique_ptr<Partition>> partitions_;
	size_t gptSectorSize_;
};

// --------------------------------------------------------
// Partition
// --------------------------------------------------------

struct Partition : public BlockDevice {
	Partition(Table &table, Guid id, Guid type,
			uint64_t start_lba, uint64_t num_sectors, arch::contiguous_pool *pool);

	async::result<std::expected<void, Error>> readSectors(uint64_t sector, arch::dma_buffer_view view) override;
	async::result<std::expected<void, Error>> writeSectors(uint64_t sector, arch::dma_buffer_view view) override;
	async::result<std::expected<void, Error>> flush() override;

	async::result<size_t> getSize() override;

	Guid id();

	Guid type();

	uint64_t startLba();

	std::atomic_bool isSwapTarget = false;

private:
	Table &_table;
	Guid _id;
	Guid _type;
	uint64_t _startLba;
	uint64_t _numSectors;
};

} } // namespace blockfs::gpt
