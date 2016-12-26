
#include <string.h>
#include <vector>

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

	uint32_t a;
	uint16_t b;
	uint16_t c;
	uint8_t d[2];
	uint8_t e[6];
};
static_assert(sizeof(Guid) == 16, "Bad sizeof(Guid)");

namespace type_guids {
	static constexpr Guid null{0, 0, 0, {0, 0}, {0, 0, 0, 0, 0, 0}};
	static constexpr Guid windowsData{0xEBD0A0A2, 0xB9E5, 0x4433, {0x87, 0xC0},
			{0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};
};

// --------------------------------------------------------
// On-disk structures
// --------------------------------------------------------

struct DiskHeader {
	uint64_t signature;
	uint32_t revision;
	uint32_t headerSize;
	uint32_t headerCheckSum;
	uint32_t reservedZero;
	uint64_t currentLba;
	uint64_t backupLba;
	uint64_t firstLba;
	uint64_t lastLba;
	uint8_t diskGuid[16];
	uint64_t startingLba;
	uint32_t numEntries;
	uint32_t entrySize;
	uint32_t tableCheckSum;
	uint8_t padding[420];
};
static_assert(sizeof(DiskHeader) == 512, "Bad GPT header struct size");

struct DiskEntry {
	Guid typeGuid;
	Guid uniqueGuid;
	uint64_t firstLba;
	uint64_t lastLba;
	uint64_t attrFlags;
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

	cofiber::future<void> parse();

	size_t numPartitions();

	Partition &getPartition(int index);

private:
	BlockDevice *device;
	std::vector<Partition> partitions;
};

// --------------------------------------------------------
// Partition
// --------------------------------------------------------

struct Partition : public BlockDevice {
	Partition(Table &table, Guid id, Guid type,
			uint64_t start_lba, uint64_t num_sectors);

	cofiber::future<void> readSectors(uint64_t sector, void *buffer,
			size_t num_sectors) override;

	Guid id();

	Guid type();

private:
	Table &_table;
	Guid _id;
	Guid _type;
	uint64_t _startLba;
	uint64_t _numSectors;
};

} } // namespace blockfs::gpt

