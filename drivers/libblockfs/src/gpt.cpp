
#include <stdlib.h>
#include <stdio.h>

#include "gpt.hpp"

namespace blockfs {
namespace gpt {

// --------------------------------------------------------
// Table
// --------------------------------------------------------

Table::Table(BlockDevice *device)
: device(device) { }

void Table::parse(frigg::CallbackPtr<void()> callback) {
	auto closure = new ParseClosure(*this, callback);
	(*closure)();
}

BlockDevice *Table::getDevice() {
	return device;
}

Partition &Table::getPartition(int index) {
	return partitions[index];
}

// --------------------------------------------------------
// Table::ParseClosure
// --------------------------------------------------------

Table::ParseClosure::ParseClosure(Table &table, frigg::CallbackPtr<void()> callback)
: table(table), callback(callback) { }

void Table::ParseClosure::operator() () {
	assert(table.getDevice()->sectorSize == 512);
	headerBuffer = malloc(512);
	assert(headerBuffer);
	table.getDevice()->readSectors(1, headerBuffer, 1,
			CALLBACK_MEMBER(this, &ParseClosure::readHeader));
}

void Table::ParseClosure::readHeader() {
	DiskHeader *header = (DiskHeader *)headerBuffer;
	assert(header->signature == 0x5452415020494645); // TODO: handle this error

	size_t table_size = header->entrySize * header->numEntries;
	size_t table_sectors = table_size / 512;
	if(!(table_size % 512))
		table_sectors++;

	tableBuffer = malloc(table_sectors * 512);
	assert(tableBuffer);
	table.getDevice()->readSectors(2, tableBuffer, table_sectors,
			CALLBACK_MEMBER(this, &ParseClosure::readTable));
}

void Table::ParseClosure::readTable() {
	DiskHeader *header = (DiskHeader *)headerBuffer;

	for(uint32_t i = 0; i < header->numEntries; i++) {
		DiskEntry *entry = (DiskEntry *)((uintptr_t)tableBuffer + i * header->entrySize);
		
		bool all_zeros = true;
		for(int j = 0; j < 16; j++)
			if(entry->typeGuid[j] != 0)
				all_zeros = false;
		if(all_zeros)
			continue;

		table.partitions.push_back(Partition(table, entry->firstLba,
				entry->lastLba - entry->firstLba + 1));
	}

	callback();
	
	free(headerBuffer);
	free(tableBuffer);
	delete this;
}

// --------------------------------------------------------
// Partition
// --------------------------------------------------------

Partition::Partition(Table &table, uint64_t start_lba, uint64_t num_sectors)
: BlockDevice(table.getDevice()->sectorSize), table(table),
		startLba(start_lba), numSectors(num_sectors) { }

void Partition::readSectors(uint64_t sector, void *buffer,
		size_t num_read_sectors, frigg::CallbackPtr<void()> callback) {
	assert(sector + num_read_sectors <= numSectors);
	table.getDevice()->readSectors(startLba + sector,
			buffer, num_read_sectors, callback);
}

} } // namespace blockfs::gpt

