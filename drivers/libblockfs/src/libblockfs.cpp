
#include <stdio.h>
#include <string.h>
#include <iostream>
#include <algorithm>
#include <sys/epoll.h>

#include <helix/ipc.hpp>
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

async::result<protocols::fs::SeekResult> seekAbs(void *object, int64_t offset) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	self->offset = offset;
	co_return self->offset;
}

async::result<protocols::fs::SeekResult> seekRel(void *object, int64_t offset) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	self->offset += offset;
	co_return self->offset;
}

async::result<protocols::fs::SeekResult> seekEof(void *object, int64_t offset) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	self->offset += offset + self->inode->fileSize();
	co_return self->offset;
}

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

async::result<protocols::fs::Error> flock(void *object, int flags) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	co_await self->inode->readyJump.async_wait();
	auto inode = self->inode;

	auto result = co_await inode->flockManager.lock(&self->flock, flags);
	co_return result;
}

async::result<protocols::fs::ReadResult> read(void *object, const char *,
		void *buffer, size_t length) {
	assert(length);

	auto self = static_cast<ext2fs::OpenFile *>(object);
	co_await self->inode->readyJump.async_wait();

	if(self->offset >= self->inode->fileSize())
		co_return 0;

	auto remaining = self->inode->fileSize() - self->offset;
	auto chunk_size = std::min(length, remaining);
	if(!chunk_size)
		co_return 0; // TODO: Return an explicit end-of-file error?

	auto chunk_offset = self->offset;
	auto map_offset = chunk_offset & ~size_t(0xFFF);
	auto map_size = (((chunk_offset & size_t(0xFFF)) + chunk_size + 0xFFF) & ~size_t(0xFFF));
	self->offset += chunk_size;

	helix::LockMemoryView lock_memory;
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(self->inode->frontalMemory),
			&lock_memory, map_offset, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{self->inode->frontalMemory},
			static_cast<ptrdiff_t>(map_offset), map_size,
			kHelMapProtRead | kHelMapDontRequireBacking};

	memcpy(buffer, reinterpret_cast<char *>(file_map.get()) + (chunk_offset - map_offset),
			chunk_size);
	co_return chunk_size;
}

async::result<protocols::fs::ReadResult> pread(void *object, int64_t offset, const char *,
		void *buffer, size_t length) {
	assert(length);

	auto self = static_cast<ext2fs::OpenFile *>(object);
	co_await self->inode->readyJump.async_wait();

	if(self->offset >= self->inode->fileSize())
		co_return 0;
	
	auto remaining = self->inode->fileSize() - offset;
	auto chunk_size = std::min(length, remaining);
	if(!chunk_size)
		co_return 0; // TODO: Return an explicit end-of-file error?

	auto chunk_offset = offset;
	auto map_offset = chunk_offset & ~size_t(0xFFF);
	auto map_size = (((chunk_offset & size_t(0xFFF)) + chunk_size + 0xFFF) & ~size_t(0xFFF));

	helix::LockMemoryView lock_memory;
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(self->inode->frontalMemory),
			&lock_memory, map_offset, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{self->inode->frontalMemory},
			static_cast<ptrdiff_t>(map_offset), map_size,
			kHelMapProtRead | kHelMapDontRequireBacking};

	memcpy(buffer, reinterpret_cast<char *>(file_map.get()) + (chunk_offset - map_offset),
			chunk_size);
	co_return chunk_size;
}

async::result<void> write(void *object, const char *,
		const void *buffer, size_t length) {
	assert(length);

	auto self = static_cast<ext2fs::OpenFile *>(object);
	co_await self->inode->fs.write(self->inode.get(), self->offset, buffer, length);
	self->offset += length;
}

async::result<helix::BorrowedDescriptor>
accessMemory(void *object) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	co_await self->inode->readyJump.async_wait();
	co_return self->inode->frontalMemory;
}

async::result<protocols::fs::ReadEntriesResult>
readEntries(void *object) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	co_return co_await self->readEntries();
}

async::result<void>
truncate(void *object, size_t size) {
	auto self = static_cast<ext2fs::OpenFile *>(object);
	co_return co_await self->inode->fs.truncate(self->inode.get(), size);
}

async::result<int> getFileFlags(void *) {
	std::cout << "libblockfs: getFileFlags is stubbed" << std::endl;
    co_return 0;
}

async::result<void> setFileFlags(void *, int) {
	std::cout << "libblockfs: setFileFlags is stubbed" << std::endl;
    co_return;
}

async::result<protocols::fs::PollResult>
poll(void *, uint64_t pastSeq, async::cancellation_token cancellation) {
	if(pastSeq)
		co_await async::suspend_indefinitely(cancellation);

	int edges = 0;
	if(!pastSeq)
		edges = EPOLLIN | EPOLLOUT;

	co_return protocols::fs::PollResult{
		1,
		edges,
		EPOLLIN | EPOLLOUT
	};
}

constexpr protocols::fs::FileOperations fileOperations {
	.seekAbs      = &seekAbs,
	.seekRel      = &seekRel,
	.seekEof      = &seekEof,
	.read         = &read,
	.pread        = &pread,
	.write        = &write,
	.readEntries  = &readEntries,
	.accessMemory = &accessMemory,
	.truncate     = &truncate,
	.flock        = &flock,
	.poll         = &poll,
	.getFileFlags = &getFileFlags,
	.setFileFlags = &setFileFlags,
};

async::result<protocols::fs::GetLinkResult> getLink(std::shared_ptr<void> object,
		std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto entry = co_await self->findEntry(name);
	if(!entry)
		co_return protocols::fs::GetLinkResult{nullptr, -1,
				protocols::fs::FileType::unknown};

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

	assert(entry->inode);
	co_return protocols::fs::GetLinkResult{fs->accessInode(entry->inode), entry->inode, type};
}

async::result<protocols::fs::GetLinkResult> link(std::shared_ptr<void> object,
		std::string name, int64_t ino) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto entry = co_await self->link(std::move(name), ino, kTypeRegular);
	if(!entry)
		co_return protocols::fs::GetLinkResult{nullptr, -1,
				protocols::fs::FileType::unknown};

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

	assert(entry->inode);
	co_return protocols::fs::GetLinkResult{fs->accessInode(entry->inode), entry->inode, type};
}

async::result<void> unlink(std::shared_ptr<void> object, std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	co_await self->unlink(std::move(name));
}

async::detached serve(smarter::shared_ptr<ext2fs::OpenFile> file,
		helix::UniqueLane local_ctrl, helix::UniqueLane local_pt) {
	async::cancellation_event cancel_pt;

	// Cancel the passthrough lane once the file line is closed.
	async::detach(protocols::fs::serveFile(std::move(local_ctrl),
			file.get(), &fileOperations), [&] {
		cancel_pt.cancel();
	});

	co_await protocols::fs::servePassthrough(std::move(local_pt),
			file, &fileOperations, cancel_pt);
}

async::result<protocols::fs::FileStats>
getStats(std::shared_ptr<void> object) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	co_await self->readyJump.async_wait();

	protocols::fs::FileStats stats;
	stats.linkCount = self->numLinks;
	stats.fileSize = self->fileSize();
	stats.mode = self->diskInode()->mode & 0xFFF;
	stats.uid = self->uid;
	stats.gid = self->gid;
	stats.accessTime = self->accessTime;
	stats.dataModifyTime = self->dataModifyTime;
	stats.anyChangeTime = self->anyChangeTime;

	co_return stats;
}

async::result<protocols::fs::OpenResult>
open(std::shared_ptr<void> object) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto file = smarter::make_shared<ext2fs::OpenFile>(self);

	helix::UniqueLane local_ctrl, remote_ctrl;
	helix::UniqueLane local_pt, remote_pt;
	std::tie(local_ctrl, remote_ctrl) = helix::createStream();
	std::tie(local_pt, remote_pt) = helix::createStream();
	serve(file, std::move(local_ctrl), std::move(local_pt));

	co_return protocols::fs::OpenResult{std::move(remote_ctrl), std::move(remote_pt)};
}

async::result<std::string> readSymlink(std::shared_ptr<void> object) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	co_await self->readyJump.async_wait();

	assert(self->fileSize() <= 60);
	std::string link(self->fileData.embedded, self->fileData.embedded + self->fileSize());
	co_return link;
}

async::result<protocols::fs::MkdirResult>
mkdir(std::shared_ptr<void> object, std::string name) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto entry = co_await self->mkdir(std::move(name));

	if(!entry)
		co_return protocols::fs::MkdirResult{nullptr, -1};

	assert(entry->inode);
	co_return protocols::fs::MkdirResult{fs->accessInode(entry->inode), entry->inode};
}

async::result<protocols::fs::Error> chmod(std::shared_ptr<void> object, int mode) {
	auto self = std::static_pointer_cast<ext2fs::Inode>(object);
	auto result = co_await self->chmod(mode);

	co_return result;
}

constexpr protocols::fs::NodeOperations nodeOperations{
	&getStats,
	&getLink,
	&link,
	&unlink,
	&open,
	&readSymlink,
	&mkdir,
	&chmod
};

} // anonymous namespace

BlockDevice::BlockDevice(size_t sector_size)
: sectorSize(sector_size) { }

async::detached servePartition(helix::UniqueLane lane) {
	std::cout << "unix device: Connection" << std::endl;

	while(true) {
		auto [accept, recv_req] = co_await helix_ng::exchangeMsgs(
			lane,
			helix_ng::accept(
				helix_ng::recvInline())
		);
		HEL_CHECK(accept.error());
		HEL_CHECK(recv_req.error());

		auto conversation = accept.descriptor();

		managarm::fs::CntRequest req;
		req.ParseFromArray(recv_req.data(), recv_req.length());
		if(req.req_type() == managarm::fs::CntReqType::DEV_MOUNT) {
			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			protocols::fs::serveNode(std::move(local_lane), fs->accessRoot(),
					&nodeOperations);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(req.req_type() == managarm::fs::CntReqType::SB_CREATE_REGULAR) {
			auto inode = co_await fs->createRegular();

			helix::UniqueLane local_lane, remote_lane;
			std::tie(local_lane, remote_lane) = helix::createStream();
			protocols::fs::serveNode(std::move(local_lane),
					inode, &nodeOperations);

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);
			resp.set_id(inode->number);
			resp.set_file_type(managarm::fs::FileType::REGULAR);

			auto ser = resp.SerializeAsString();
			auto [send_resp, push_node] = co_await helix_ng::exchangeMsgs(
				conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()),
				helix_ng::pushDescriptor(remote_lane)
			);
			HEL_CHECK(send_resp.error());
			HEL_CHECK(push_node.error());
		}else if(req.req_type() == managarm::fs::CntReqType::RENAME) {
			auto oldInode = fs->accessInode(req.inode_source());
			auto newInode = fs->accessInode(req.inode_target());

			auto old_file = co_await oldInode->findEntry(req.old_name());
			if(old_file) {
				if(co_await newInode->findEntry(req.new_name()) != std::nullopt) {
					co_await newInode->unlink(req.new_name());
				}
				co_await newInode->link(req.new_name(), old_file.value().inode, old_file.value().fileType);
			} else {
				managarm::fs::SvrResponse resp;
				resp.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

				auto ser = resp.SerializeAsString();
				auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
					helix_ng::sendBuffer(ser.data(), ser.size()));
				HEL_CHECK(send_resp.error());
			}

			co_await oldInode->unlink(req.old_name());

			managarm::fs::SvrResponse resp;
			resp.set_error(managarm::fs::Errors::SUCCESS);

			auto ser = resp.SerializeAsString();
			auto [send_resp] = co_await helix_ng::exchangeMsgs(conversation,
				helix_ng::sendBuffer(ser.data(), ser.size()));
			HEL_CHECK(send_resp.error());
		}else{
			throw std::runtime_error("Unexpected request type");
		}
	}
}

async::detached runDevice(BlockDevice *device) {
	table = new gpt::Table(device);
	co_await table->parse();

	for(size_t i = 0; i < table->numPartitions(); ++i) {
		auto type = table->getPartition(i).type();
		printf("Partition %lu, type: %.8X-%.4X-%.4X-%.2X%.2X-%.2X%.2X%.2X%.2X%.2X%.2X\n",
				i, type.a, type.b, type.c, type.d[0], type.d[1],
				type.e[0], type.e[1], type.e[2], type.e[3], type.e[4], type.e[5]);

		if(type != gpt::type_guids::windowsData)
			continue;
		printf("It's a Windows data partition!\n");

		fs = new ext2fs::FileSystem(&table->getPartition(i));
		co_await fs->init();
		printf("ext2fs is ready!\n");

		// Create an mbus object for the partition.
		auto root = co_await mbus::Instance::global().getRoot();

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

		co_await root.createObject("partition", descriptor, std::move(handler));
	}
}

} // namespace blockfs
