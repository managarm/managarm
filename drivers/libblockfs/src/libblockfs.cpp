
#include <stdio.h>
#include <iostream>

#include <blockfs.hpp>
#include "gpt.hpp"
#include "ext2fs.hpp"

namespace blockfs {

// TODO: Support more than one table.
gpt::Table *table;

BlockDevice::BlockDevice(size_t sector_size)
: sectorSize(sector_size) { }

/*ext2fs::FileSystem *fs;
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

COFIBER_ROUTINE(cofiber::no_future, runDevice(BlockDevice *device), ([=] {
	table = new gpt::Table(device);
	COFIBER_AWAIT table->parse();

	for(size_t i = 0; i < table->numPartitions(); ++i) {
		auto type = table->getPartition(i).type();
		printf("Partition %lu, type: %.8X-%.4X-%.4X-%.2X%.2X-%.2X%.2X%.2X%.2X%.2X%.2X\n",
				i, type.a, type.b, type.c, type.d[0], type.d[1],
				type.e[0], type.e[1], type.e[2], type.e[3], type.e[4], type.e[5]);

		if(type != gpt::type_guids::windowsData)
			continue;
		printf("It's a Windows data partition!\n");
	}
}))

} // namespace blockfs

