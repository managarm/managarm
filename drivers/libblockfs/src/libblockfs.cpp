
#include <stdio.h>

#include <blockfs.hpp>
#include "gpt.hpp"
#include "ext2fs.hpp"

namespace blockfs {

BlockDevice::BlockDevice(size_t sector_size)
: sectorSize(sector_size) { }

/*helx::EventHub *theEventHub;
gpt::Table *table;
ext2fs::FileSystem *fs;
ext2fs::Client *client;

void haveMbus(void *object) {
	printf("ext2fs initialized successfully\n");
}

void haveFs(void *object) {
	client = new ext2fs::Client(*theEventHub, *fs);
	client->init(CALLBACK_STATIC(nullptr, &haveMbus));
}

void havePartitions(void *object) {
	fs = new ext2fs::FileSystem(*theEventHub, &table->getPartition(1));
	fs->init(CALLBACK_STATIC(nullptr, &haveFs));
}*/

void runDevice(BlockDevice *device) {
	printf("FIXME: Fix runDevice()\n");
/*	theEventHub = &event_hub;

	table = new gpt::Table(device);
	table->parse(CALLBACK_STATIC(nullptr, &havePartitions));*/
}

} // namespace blockfs

