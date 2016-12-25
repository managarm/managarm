
#include <vector>

#include <blockfs.hpp>

namespace blockfs {
namespace gpt {

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
	uint8_t typeGuid[16];
	uint8_t uniqueGuid[16];
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

/*struct Table {
public:
	Table(BlockDevice *device);

	BlockDevice *getDevice();

	void parse(frigg::CallbackPtr<void()> callback);

	Partition &getPartition(int index);

private:

	struct ParseClosure {
		ParseClosure(Table &table, frigg::CallbackPtr<void()> callback);

		void operator() ();
	
	private:
		void readHeader();
		void readTable();

		Table &table;
		void *headerBuffer;
		void *tableBuffer;
		frigg::CallbackPtr<void()> callback;
	};

	BlockDevice *device;
	std::vector<Partition> partitions;
};

// --------------------------------------------------------
// Partition
// --------------------------------------------------------

struct Partition : public BlockDevice {
	Partition(Table &table, uint64_t start_lba, uint64_t num_sectors);

	cofiber::future<void> readSectors(uint64_t sector, void *buffer,
			size_t num_sectors) override;
	
	Table &table;
	uint64_t startLba;
	uint64_t numSectors;
};*/

} } // namespace blockfs::gpt

