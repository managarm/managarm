
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

COFIBER_ROUTINE(async::result<int64_t>, seekAbs(std::shared_ptr<void> object,
		int64_t offset), ([=] {
	auto self = std::static_pointer_cast<ext2fs::OpenFile>(object);
	self->offset = offset;
	COFIBER_RETURN(self->offset);
}))

COFIBER_ROUTINE(async::result<size_t>, read(std::shared_ptr<void> object,
		void *buffer, size_t length), ([=] {
	auto self = std::static_pointer_cast<ext2fs::OpenFile>(object);
	COFIBER_AWAIT self->inode->readyJump.async_wait();

	assert(self->offset <= self->inode->fileSize);
	auto remaining = self->inode->fileSize - self->offset;
	auto chunk_size = std::min(length, remaining);
	assert(chunk_size);

	auto chunk_offset = self->offset;
	auto map_offset = chunk_offset & ~size_t(0xFFF);
	auto map_size = ((chunk_offset + chunk_size) & ~size_t(0xFFF)) - map_offset + 0x1000;
	self->offset += chunk_size;

	helix::LockMemory lock_memory;
	auto &&submit = helix::submitLockMemory(helix::BorrowedDescriptor(self->inode->frontalMemory),
			&lock_memory, map_offset, map_size, helix::Dispatcher::global());
	COFIBER_AWAIT(submit.async_wait());
	HEL_CHECK(lock_memory.error());

	// TODO: Use a RAII mapping class to get rid of the mapping on return.
	// map the page cache into the address space
	void *window;
	HEL_CHECK(helMapMemory(self->inode->frontalMemory, kHelNullHandle,
			nullptr, map_offset, map_size, kHelMapProtRead | kHelMapDontRequireBacking, &window));

	memcpy(buffer, (char *)window + (chunk_offset - map_offset), chunk_size);
	COFIBER_RETURN(chunk_size);
}))

async::result<void> write(std::shared_ptr<void> object, const void *buffer, size_t length) {
	throw std::runtime_error("write not implemented");
}

COFIBER_ROUTINE(async::result<protocols::fs::AccessMemoryResult>,
		accessMemory(std::shared_ptr<void> object, uint64_t offset, size_t size), ([=] {
	auto self = std::static_pointer_cast<ext2fs::OpenFile>(object);
	COFIBER_AWAIT self->inode->readyJump.async_wait();
	assert(offset + size <= self->inode->fileSize);
	COFIBER_RETURN(std::make_pair(helix::BorrowedDescriptor{self->inode->frontalMemory}, offset));
}))

constexpr auto fileOperations = protocols::fs::FileOperations{}
	.withSeekAbs(&seekAbs)
	.withRead(&read)
	.withWrite(&write)
	.withAccessMemory(&accessMemory);

COFIBER_ROUTINE(async::result<protocols::fs::GetLinkResult>, getLink(std::shared_ptr<void> object,
		std::string name), ([=] {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto entry = COFIBER_AWAIT(self->findEntry(name));
	if(!entry)
		COFIBER_RETURN((protocols::fs::GetLinkResult{nullptr, -1, protocols::fs::FileType::unknown}));

	protocols::fs::FileType type;
	switch(entry->fileType) {
	case kTypeDirectory:
		type = protocols::fs::FileType::directory;
		break;
	case kTypeRegular:
		type = protocols::fs::FileType::regular;
		break;
	case kTypeSymlink:
		type = protocols::fs::FileType::symlink;
		break;
	default:
		throw std::runtime_error("Unexpected file type");
	}

	COFIBER_RETURN((protocols::fs::GetLinkResult{fs->accessInode(entry->inode), entry->inode, type}));
}))

COFIBER_ROUTINE(async::result<std::shared_ptr<void>>, open(std::shared_ptr<void> object), ([=] {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	COFIBER_RETURN(std::make_shared<ext2fs::OpenFile>(self));
}))

COFIBER_ROUTINE(async::result<std::string>, readSymlink(std::shared_ptr<void> object), ([=] {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	COFIBER_AWAIT self->readyJump.async_wait();

	assert(self->fileSize <= 60);
	std::string link(self->fileData.embedded, self->fileData.embedded + self->fileSize);
	COFIBER_RETURN(link);
}))

constexpr protocols::fs::NodeOperations nodeOperations{
	&getLink,
	&open,
	&readSymlink
};

} // anonymous namespace

BlockDevice::BlockDevice(size_t sector_size)
: sectorSize(sector_size) { }

COFIBER_ROUTINE(cofiber::no_future, servePartition(helix::UniqueLane p),
		([lane = std::move(p)] {
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		helix::Accept accept;
		helix::RecvInline recv_req;

		auto &&header = helix::submitAsync(lane, helix::Dispatcher::global(),
				helix::action(&accept, kHelItemAncillary),
				helix::action(&recv_req));
		COFIBER_AWAIT header.async_wait();
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());
		
		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::DEV_MOUNT) {
			helix::SendBuffer send_resp;
			helix::PushDescriptor push_node;
			
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			protocols::fs::serveNode(std::move(local_lane), fs->accessRoot(),
					&nodeOperations, &fileOperations);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto &&transmit = helix::submitAsync(conversation, helix::Dispatcher::global(),
					helix::action(&send_resp, ser.data(), ser.size(), kHelItemChain),
					helix::action(&push_node, remote_lane));
			COFIBER_AWAIT transmit.async_wait();
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
		
		mbus::Properties descriptor{
			{"unix.devtype", mbus::StringItem{"block"}},
			{"unix.devname", mbus::StringItem{"sda0"}}
		};

		auto handler = mbus::ObjectHandler{}
		.withBind([] () -> async::result<helix::UniqueDescriptor> {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			servePartition(std::move(local_lane));

			async::promise<helix::UniqueDescriptor> promise;
			promise.set_value(std::move(remote_lane));
			return promise.async_get();
		});

		COFIBER_AWAIT root.createObject("partition", descriptor, std::move(handler));
	}
}))

} // namespace blockfs

