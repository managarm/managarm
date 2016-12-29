
#include <stdio.h>
#include <string.h>
#include <iostream>

#include <helix/ipc.hpp>
#include <protocols/mbus/client.hpp>

#include <blockfs.hpp>
#include "gpt.hpp"
#include "ext2fs.hpp"

namespace blockfs {

// TODO: Support more than one table.
gpt::Table *table;
ext2fs::FileSystem *fs;
//ext2fs::Client *client;

BlockDevice::BlockDevice(size_t sector_size)
: sectorSize(sector_size) { }

/*void haveFs(void *object) {
	client = new ext2fs::Client(*theEventHub, *fs);
	client->init(CALLBACK_STATIC(nullptr, &haveMbus));
}
*/

COFIBER_ROUTINE(cofiber::no_future, servePartition(helix::UniqueLane p),
		([lane = std::move(p)] {
	std::cout << "unix device: Connection" << std::endl;
}))

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
		
		fs = new ext2fs::FileSystem(&table->getPartition(i));
		COFIBER_AWAIT fs->init();
		printf("ext2fs is ready!\n");

		// Create an mbus object for the partition.
		auto root = COFIBER_AWAIT mbus::Instance::global().getRoot();
		
		std::unordered_map<std::string, std::string> descriptor {
			{ "unix.devtype", "block" }
		};
		auto object = COFIBER_AWAIT root.createObject("partition", descriptor,
				[=] (mbus::AnyQuery query) -> async::result<helix::UniqueDescriptor> {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			servePartition(std::move(local_lane));

			async::promise<helix::UniqueDescriptor> promise;
			promise.set_value(std::move(remote_lane));
			return promise.async_get();
		});
	}
}))

} // namespace blockfs

