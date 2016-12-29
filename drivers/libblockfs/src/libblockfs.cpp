
#include <stdio.h>
#include <string.h>
#include <iostream>

#include <helix/ipc.hpp>
#include <helix/await.hpp>
#include <protocols/fs/server.hpp>
#include <protocols/mbus/client.hpp>

#include <blockfs.hpp>
#include "gpt.hpp"
#include "ext2fs.hpp"
#include "fs.pb.h"

namespace blockfs {

// TODO: Support more than one table.
gpt::Table *table;
ext2fs::FileSystem *fs;

namespace {

async::result<std::shared_ptr<void>> getLink(std::shared_ptr<void> object, std::string name);

constexpr protocols::fs::NodeOperations nodeOperations{
	&getLink
};

COFIBER_ROUTINE(async::result<std::shared_ptr<void>>, getLink(std::shared_ptr<void> object,
		std::string name), ([=] {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto entry = COFIBER_AWAIT(self->findEntry(name));
	assert(entry);
	COFIBER_RETURN(fs->accessInode(entry->inode));
}))

} // anonymous namespace

BlockDevice::BlockDevice(size_t sector_size)
: sectorSize(sector_size) { }

COFIBER_ROUTINE(cofiber::no_future, servePartition(helix::UniqueLane p),
		([lane = std::move(p)] {
	using M = helix::AwaitMechanism;
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		helix::Accept<M> accept;
		helix::RecvInline<M> recv_req;

		helix::submitAsync(lane, {
			helix::action(&accept, kHelItemAncillary),
			helix::action(&recv_req)
		}, helix::Dispatcher::global());
		COFIBER_AWAIT accept.future();
		COFIBER_AWAIT recv_req.future();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::DEV_MOUNT) {
			helix::SendBuffer<M> send_resp;
			helix::PushDescriptor<M> push_node;
			
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			protocols::fs::serveNode(std::move(local_lane), fs->accessRoot(), &nodeOperations);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			helix::submitAsync(conversation, {
				helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
				helix::action(&push_node, remote_lane)
			}, helix::Dispatcher::global());
			COFIBER_AWAIT send_resp.future();
			COFIBER_AWAIT push_node.future();
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
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

