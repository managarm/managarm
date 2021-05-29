
#include <stdlib.h>
#include <iostream>

#include "gpt.hpp"

namespace blockfs {
namespace gpt {

// --------------------------------------------------------
// Table
// --------------------------------------------------------

Table::Table(BlockDevice *device)
: device(device) { }

async::result<void> Table::parse() {
	assert(getDevice()->sectorSize == 512);

	auto header_buffer = malloc(512);
	assert(header_buffer);
	co_await getDevice()->readSectors(1, header_buffer, 1);

	DiskHeader *header = (DiskHeader *)header_buffer;
	assert(header->signature == 0x5452415020494645); // TODO: handle this error

	size_t table_size = header->entrySize * header->numEntries;
	size_t table_sectors = table_size / 512;
	if(!(table_size % 512))
		table_sectors++;

	auto table_buffer = malloc(table_sectors * 512);
	assert(table_buffer);
	co_await getDevice()->readSectors(2, table_buffer, table_sectors);

	for(uint32_t i = 0; i < header->numEntries; i++) {
		DiskEntry *entry = (DiskEntry *)((char *)table_buffer + i * header->entrySize);

		if(entry->typeGuid == type_guids::null)
			continue;

		partitions.push_back(Partition{*this, entry->uniqueGuid, entry->typeGuid,
				entry->firstLba, entry->lastLba - entry->firstLba + 1});
	}

	free(header_buffer);
	free(table_buffer);
}

BlockDevice *Table::getDevice() {
	return device;
}

size_t Table::numPartitions() {
	return partitions.size();
}

Partition &Table::getPartition(int index) {
	return partitions[index];
}

// --------------------------------------------------------
// Partition
// --------------------------------------------------------

Partition::Partition(Table &table, Guid id, Guid type,
		uint64_t start_lba, uint64_t num_sectors)
: BlockDevice(table.getDevice()->sectorSize, table.getDevice()->parentId), _table(table),
	_id(id), _type(type), _startLba(start_lba), _numSectors(num_sectors) { }

Guid Partition::type() {
	return _type;
}

async::result<void> Partition::readSectors(uint64_t sector, void *buffer, size_t count) {
	assert(sector + count <= _numSectors);
	return _table.getDevice()->readSectors(_startLba + sector,
			buffer, count);
}

async::result<void> Partition::writeSectors(uint64_t sector, const void *buffer, size_t count) {
	assert(sector + count <= _numSectors);
	return _table.getDevice()->writeSectors(_startLba + sector,
			buffer, count);
}

} } // namespace blockfs::gpt

