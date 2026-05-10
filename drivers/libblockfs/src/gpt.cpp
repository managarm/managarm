
#include <stdlib.h>
#include <print>
#include <array>

#include "gpt.hpp"

namespace blockfs {
namespace gpt {

namespace {
	constexpr std::array<size_t, 4> commonSectorSizes{512, 2048, 4096, 8192};

	constexpr size_t gptSectorNumber = 1;
}

// --------------------------------------------------------
// Table
// --------------------------------------------------------

Table::Table(BlockDevice *device)
: device_(device), gptSectorSize_{device->sectorSize} { }

async::result<std::pair<arch::dma_buffer, DiskHeader *>> Table::probeSectorSize_(size_t sectorSize) {
	size_t deviceGptSector = (sectorSize * gptSectorNumber) / getDevice()->sectorSize;
	size_t deviceGptOffset = (sectorSize * gptSectorNumber) % getDevice()->sectorSize;

	size_t deviceSectors = (sectorSize + deviceGptOffset + getDevice()->sectorSize - 1) / getDevice()->sectorSize;

	arch::dma_buffer headerBuffer{getDevice()->pagePool, deviceSectors * getDevice()->sectorSize};
	co_await getDevice()->readSectors(deviceGptSector, headerBuffer);

	DiskHeader *header = reinterpret_cast<DiskHeader *>(static_cast<std::byte *>(headerBuffer.data()) + deviceGptOffset);
	if (header->signature == 0x5452415020494645)
		co_return std::make_pair(std::move(headerBuffer), header);

	co_return std::make_pair(arch::dma_buffer{}, nullptr);
}

async::result<void> Table::parse() {
	// probe for the gpt header
	auto header = co_await probeSectorSize_(getDevice()->sectorSize);
	if (!header.first.size()) {
		// try other sizes
		for (auto size : commonSectorSizes) {
			if (size == getDevice()->sectorSize)
				continue;

			header = co_await probeSectorSize_(size);
			if (header.first.size()) {
				gptSectorSize_ = size;
				break;
			}
		}

		// TODO: handle this error
		assert(header.first.size());

		std::println(std::cout, "libblockfs: using non-native gpt sector size {}",
			gptSectorSize_);
	}

	size_t deviceTableSector = (header.second->entryTableLba * gptSectorSize_) / getDevice()->sectorSize;
	size_t deviceTableOffset = (header.second->entryTableLba * gptSectorSize_) % getDevice()->sectorSize;

	size_t tableSize = header.second->entrySize * header.second->numEntries;
	size_t tableSectors = (tableSize + deviceTableOffset + getDevice()->sectorSize - 1) / getDevice()->sectorSize;

	arch::dma_buffer tableBuffer{getDevice()->pagePool, tableSectors * getDevice()->sectorSize};
	co_await getDevice()->readSectors(deviceTableSector, tableBuffer);

	for (uint32_t i = 0; i < header.second->numEntries; i++) {
		DiskEntry *entry = reinterpret_cast<DiskEntry *>(
		    static_cast<std::byte *>(tableBuffer.data()) + deviceTableOffset
		    + i * header.second->entrySize
		);

		if (entry->typeGuid == type_guids::null)
			continue;

		size_t offset = entry->firstLba * gptSectorSize_;
		size_t size = (entry->lastLba - entry->firstLba + 1) * gptSectorSize_;

		size_t deviceSector = offset / getDevice()->sectorSize;
		size_t deviceSectorOffset = offset % getDevice()->sectorSize;

		assert(deviceSectorOffset == 0);
		assert(size % getDevice()->sectorSize == 0);

		partitions_.push_back(Partition{*this, entry->uniqueGuid, entry->typeGuid,
				deviceSector, size / getDevice()->sectorSize, this->getDevice()->pagePool});
	}
}

BlockDevice *Table::getDevice() {
	return device_;
}

size_t Table::numPartitions() {
	return partitions_.size();
}

Partition &Table::getPartition(int index) {
	return partitions_[index];
}

// --------------------------------------------------------
// Partition
// --------------------------------------------------------

Partition::Partition(
    Table &table,
    Guid id,
    Guid type,
    uint64_t start_lba,
    uint64_t num_sectors,
    arch::contiguous_pool *pool
)
: BlockDevice(table.getDevice()->sectorSize, table.getDevice()->parentId, pool),
  _table(table),
  _id(id),
  _type(type),
  _startLba(start_lba),
  _numSectors(num_sectors) {}

Guid Partition::type() {
	return _type;
}

async::result<void> Partition::readSectors(uint64_t sector, arch::dma_buffer_view view) {
	assert(sector + (view.size() / sectorSize) <= _numSectors);
	return _table.getDevice()->readSectors(_startLba + sector, view);
}

async::result<void> Partition::writeSectors(uint64_t sector, arch::dma_buffer_view view) {
	assert(sector + (view.size() / sectorSize) <= _numSectors);
	return _table.getDevice()->writeSectors(_startLba + sector, view);
}

async::result<size_t> Partition::getSize() {
	co_return _numSectors * sectorSize;
}

} } // namespace blockfs::gpt

