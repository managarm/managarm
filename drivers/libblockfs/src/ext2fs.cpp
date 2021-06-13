
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <iostream>
#include <sys/stat.h>

#include <async/result.hpp>
#include <helix/ipc.hpp>
#include <helix/memory.hpp>

#include <array>

#include "ext2fs.hpp"

namespace blockfs {
namespace ext2fs {

namespace {
	constexpr bool logSuperblock = true;

	constexpr int pageShift = 12;
	constexpr size_t pageSize = size_t{1} << pageShift;
}

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

Inode::Inode(FileSystem &fs, uint32_t number)
: fs(fs), number(number), isReady(false) { }

void Inode::setFileSize(size_t size) {
	assert(!(size & ~uint64_t(0xFFFFFFFF)));
	diskInode()->size = size;
}

async::result<frg::expected<protocols::fs::Error, std::optional<DirEntry>>>
Inode::findEntry(std::string name) {
	co_await readyJump.wait();

	if(fileType != kTypeDirectory)
		co_return protocols::fs::Error::notDirectory;
	assert(fileMapping.size() == fileSize());

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Read the directory structure.
	uintptr_t offset = 0;
	while(offset < fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= fileSize());
		auto disk_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(fileMapping.get()) + offset);
		assert(disk_entry->recordLength);

		if(disk_entry->inode
				&& name.length() == disk_entry->nameLength
				&& !memcmp(disk_entry->name, name.data(), name.length())) {
			DirEntry entry;
			entry.inode = disk_entry->inode;

			switch(disk_entry->fileType) {
			case EXT2_FT_REG_FILE:
				entry.fileType = kTypeRegular; break;
			case EXT2_FT_DIR:
				entry.fileType = kTypeDirectory; break;
			case EXT2_FT_SYMLINK:
				entry.fileType = kTypeSymlink; break;
			default:
				entry.fileType = kTypeNone;
			}

			co_return entry;
		}

		offset += disk_entry->recordLength;
	}
	assert(offset == fileSize());

	co_return std::nullopt;
}

async::result<std::optional<DirEntry>>
Inode::link(std::string name, int64_t ino, blockfs::FileType type) {
	assert(!name.empty() && name != "." && name != "..");
	assert(ino);

	co_await readyJump.wait();

	assert(fileType == kTypeDirectory);
	assert(fileMapping.size() == fileSize());

	// Lock the mapping into memory before calling this function.
	auto appendDirEntry = [&](size_t offset, size_t length)
			-> async::result<std::optional<DirEntry>> {
		auto diskEntry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(fileMapping.get()) + offset);
		memset(diskEntry, 0, sizeof(DiskDirEntry));
		diskEntry->inode = ino;
		diskEntry->recordLength = length;
		diskEntry->nameLength = name.length();
		switch (type) {
			case kTypeRegular:
				diskEntry->fileType = EXT2_FT_REG_FILE;
				break;
			case kTypeDirectory:
				diskEntry->fileType = EXT2_FT_DIR;
				break;
			case kTypeSymlink:
				diskEntry->fileType = EXT2_FT_SYMLINK;
				break;
			default:
				throw std::runtime_error("unexpected type");
		}
		memcpy(diskEntry->name, name.data(), name.length() + 1);

		// Flush the data to disk.
		// TODO: It would be enough to flush only one or two pages here.
		auto syncDir = co_await helix_ng::synchronizeSpace(
				helix::BorrowedDescriptor{kHelNullHandle}, fileMapping.get(), fileSize());
		HEL_CHECK(syncDir.error());

		// Increment the target's link count.
		auto target = fs.accessInode(ino);
		co_await target->readyJump.wait();
		target->diskInode()->linksCount++;

		// Flush the target inode to disk.
		auto syncInode = co_await helix_ng::synchronizeSpace(
				helix::BorrowedDescriptor{kHelNullHandle},
				target->diskMapping.get(), fs.inodeSize);
		HEL_CHECK(syncInode.error());

		DirEntry entry;
		entry.inode = ino;
		entry.fileType = type;
		co_return entry;
	};

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Space required for the new directory entry.
	// We use name.size() + 1 for the entry name length to account for the null terminator
	auto required = (sizeof(DiskDirEntry) + name.size() + 1 + 3) & ~size_t(3);

	// Walk the directory structure.
	uintptr_t offset = 0;
	while(offset < fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= fileSize());
		auto previous_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(fileMapping.get()) + offset);
		assert(previous_entry->recordLength);

		// Calculate available space after we contract previous_entry.
		auto contracted = (sizeof(DiskDirEntry) + previous_entry->nameLength + 3) & ~size_t(3);
		assert(previous_entry->recordLength >= contracted);
		auto available = previous_entry->recordLength - contracted;

		// Check whether we can shrink previous_entry and insert a new entry after it.
		if(available >= required) {
			// Update the existing dentry.
			previous_entry->recordLength = contracted;

			co_return co_await appendDirEntry(offset + contracted, available);
		}

		offset += previous_entry->recordLength;
	}
	assert(offset == fileSize());

	// If we made it this far, we ran out of space in the directory. Resize it.
	auto blockOffset = (offset & ~(fs.blockSize - 1)) >> fs.blockShift;
	auto newSize = (offset + fs.blockSize + 0xFFF) & ~size_t(0xFFF);
	setFileSize(newSize);
	co_await fs.assignDataBlocks(this, blockOffset, 1);
	HEL_CHECK(helResizeMemory(backingMemory, newSize));
	fileMapping = helix::Mapping{helix::BorrowedDescriptor{frontalMemory},
			0, newSize,
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

	// Now append the entry that we couldn't add before.
	{
		helix::LockMemoryView lock_memory;
		auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
				&lock_memory,
				0, newSize, helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(lock_memory.error());

		co_return co_await appendDirEntry(offset, fileSize() - offset);
	}
}

async::result<frg::expected<protocols::fs::Error>> Inode::unlink(std::string name) {
	assert(!name.empty() && name != "." && name != "..");

	co_await readyJump.wait();

	if(fileType != kTypeDirectory)
		co_return protocols::fs::Error::notDirectory;
	assert(fileMapping.size() == fileSize());

	helix::LockMemoryView lock_memory;
	auto map_size = (fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(frontalMemory),
			&lock_memory,
			0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Read the directory structure.
	DiskDirEntry *previous_entry = nullptr;
	uintptr_t offset = 0;
	while(offset < fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= fileSize());
		auto disk_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(fileMapping.get()) + offset);
		assert(disk_entry->recordLength);

		if(disk_entry->inode
				&& name.length() == disk_entry->nameLength
				&& !memcmp(disk_entry->name, name.data(), name.length())) {
			// The directory should start with "." and "..". As those entries are never deleted,
			// we can assume that a previous entry exists.
			assert(previous_entry);
			previous_entry->recordLength += disk_entry->recordLength;

			// Flush the data to disk.
			// TODO: It would be enough to flush only one or two pages here.
			auto syncDir = co_await helix_ng::synchronizeSpace(
					helix::BorrowedDescriptor{kHelNullHandle}, fileMapping.get(), fileSize());
			HEL_CHECK(syncDir.error());

			// Decrement the inode's link count
			auto target = fs.accessInode(disk_entry->inode);
			co_await target->readyJump.wait();
			target->diskInode()->linksCount--;
			auto syncInode = co_await helix_ng::synchronizeSpace(
					helix::BorrowedDescriptor{kHelNullHandle},
					target->diskMapping.get(), fs.inodeSize);
			HEL_CHECK(syncInode.error());

			co_return {};
		}

		offset += disk_entry->recordLength;
		previous_entry = disk_entry;
	}
	assert(offset == fileSize());

	co_return protocols::fs::Error::fileNotFound;
}

async::result<std::optional<DirEntry>> Inode::mkdir(std::string name) {
	assert(!name.empty() && name != "." && name != "..");

	co_await readyJump.wait();

	auto dirNode = co_await fs.createDirectory();
	co_await dirNode->readyJump.wait();

	co_await fs.assignDataBlocks(dirNode.get(), 0, 1);

	dirNode->setFileSize(fs.blockSize);
	HEL_CHECK(helResizeMemory(dirNode->backingMemory,
			(fs.blockSize + 0xFFF) & ~size_t(0xFFF)));
	dirNode->fileMapping = helix::Mapping{helix::BorrowedDescriptor{dirNode->frontalMemory},
			0, fs.blockSize,
			kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

	helix::LockMemoryView lockMemory;
	auto mapSize = (dirNode->fileSize() + 0xFFF) & ~size_t(0xFFF);
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(dirNode->frontalMemory),
			&lockMemory,
			0, mapSize, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lockMemory.error());

	// XXX: this is a hack to make the directory accessible under
	// OSes that respect the permissions, this means "drwxr-xr-x"
	dirNode->diskInode()->mode = 0x41ED;
	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			dirNode->diskMapping.get(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	size_t offset = 0;
	auto dotEntry = reinterpret_cast<DiskDirEntry *>(dirNode->fileMapping.get());
	offset += (sizeof(DiskDirEntry) + 2 + 3) & ~size_t(3);

	dirNode->diskInode()->linksCount++;
	dotEntry->inode = dirNode->number;
	dotEntry->recordLength = offset;
	dotEntry->nameLength = 1;
	dotEntry->fileType = EXT2_FT_DIR;
	memcpy(dotEntry->name, ".", 2);

	auto dotDotEntry = reinterpret_cast<DiskDirEntry *>(
			reinterpret_cast<char *>(dirNode->fileMapping.get()) + offset);

	diskInode()->linksCount++;
	dotDotEntry->inode = number;
	dotDotEntry->recordLength = dirNode->fileSize() - offset;
	dotDotEntry->nameLength = 2;
	dotDotEntry->fileType = EXT2_FT_DIR;
	memcpy(dotDotEntry->name, "..", 3);

	// Synchronize this inode to update the linksCount
	syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			diskMapping.get(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	// Synchronize the data blocks
	syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			dirNode->fileMapping.get(), dirNode->fileSize());
	HEL_CHECK(syncInode.error());

	co_return co_await link(name, dirNode->number, kTypeDirectory);
}

async::result<std::optional<DirEntry>> Inode::symlink(std::string name, std::string target) {
	assert(!name.empty() && name != "." && name != "..");

	co_await readyJump.wait();

	auto newNode = co_await fs.createSymlink();
	co_await newNode->readyJump.wait();

	assert(target.size() <= 60); // TODO: implement this case!
	newNode->setFileSize(target.size());
	memcpy(newNode->diskInode()->data.embedded, target.data(), target.size());

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			newNode->diskMapping.get(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	co_return co_await link(name, newNode->number, kTypeSymlink);
}

async::result<protocols::fs::Error> Inode::chmod(int mode) {
	co_await readyJump.wait();

	diskInode()->mode = (diskInode()->mode & 0xFFFFF000) | mode;

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			diskMapping.get(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	co_return protocols::fs::Error::none;
}

async::result<protocols::fs::Error> Inode::utimensat(uint64_t atime_sec, uint64_t atime_nsec, uint64_t mtime_sec, uint64_t mtime_nsec) {
	std::cout << "\e[31m" "ext2fs: utimensat() only supports setting atime and mtime to current time" "\e[39m" << std::endl;
	
	co_await readyJump.wait();

	if(atime_sec != UTIME_NOW || atime_nsec != UTIME_NOW || mtime_sec != UTIME_NOW || mtime_nsec != UTIME_NOW) {
		// TODO: Properly implement setting the time to arbitrary values
		std::cout << "\e[31m" "ext2fs: utimensat() unsupported mode called (not UTIME_NOW for all fields)" "\e[39m" << std::endl;
		co_return protocols::fs::Error::none;
	}

	struct timespec time;
	// TODO: Move to CLOCK_REALTIME when supported
	clock_gettime(CLOCK_MONOTONIC, &time);
	diskInode()->atime = time.tv_sec;
	diskInode()->mtime = time.tv_sec;

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			diskMapping.get(), fs.inodeSize);
	HEL_CHECK(syncInode.error());

	co_return protocols::fs::Error::none;
}

// --------------------------------------------------------
// FileSystem
// --------------------------------------------------------

FileSystem::FileSystem(BlockDevice *device)
: device(device) {
}

async::result<void> FileSystem::init() {
	std::vector<uint8_t> buffer(1024);
	co_await device->readSectors(2, buffer.data(), 2);

	DiskSuperblock sb;
	memcpy(&sb, buffer.data(), sizeof(DiskSuperblock));
	assert(sb.magic == 0xEF53);

	inodeSize = sb.inodeSize;
	blockShift = 10 + sb.logBlockSize;
	blockSize = 1024 << sb.logBlockSize;
	blockPagesShift = blockShift < pageShift ? pageShift : blockShift;
	sectorsPerBlock = blockSize / 512;
	blocksPerGroup = sb.blocksPerGroup;
	inodesPerGroup = sb.inodesPerGroup;
	blocksCount = sb.blocksCount;
	inodesCount = sb.inodesCount;
	numBlockGroups = (sb.blocksCount + (sb.blocksPerGroup - 1)) / sb.blocksPerGroup;

	if(logSuperblock) {
		std::cout << "ext2fs: Revision is: " << sb.revLevel << std::endl;
		std::cout << "ext2fs: Block size is: " << blockSize << std::endl;
		std::cout << "ext2fs:     There are " << sb.blocksCount << " blocks" << std::endl;
		std::cout << "ext2fs: Inode size is: " << inodeSize << std::endl;
		std::cout << "ext2fs:     There are " << sb.inodesCount << " blocks" << std::endl;
		std::cout << "ext2fs:     First available inode is: " << sb.firstIno << std::endl;
		std::cout << "ext2fs: Optional features: " << sb.featureCompat
				<< ", w-required features: " << sb.featureRoCompat
				<< ", r/w-required features: " << sb.featureIncompat << std::endl;
		std::cout << "ext2fs: There are " << numBlockGroups << " block groups" << std::endl;
		std::cout << "ext2fs:     Blocks per group: " << blocksPerGroup << std::endl;
		std::cout << "ext2fs:     Inodes per group: " << inodesPerGroup << std::endl;
	}

	blockGroupDescriptorBuffer.resize((numBlockGroups * sizeof(DiskGroupDesc) + 511) & ~size_t(511));
	bgdt = (DiskGroupDesc *)blockGroupDescriptorBuffer.data();

	auto bgdt_offset = (2048 + blockSize - 1) & ~size_t(blockSize - 1);
	co_await device->readSectors((bgdt_offset >> blockShift) * sectorsPerBlock,
			blockGroupDescriptorBuffer.data(), blockGroupDescriptorBuffer.size() / 512);

	// Create memory bundles to manage the block and inode bitmaps.
	HelHandle block_bitmap_frontal, inode_bitmap_frontal;
	HelHandle block_bitmap_backing, inode_bitmap_backing;
	HEL_CHECK(helCreateManagedMemory(numBlockGroups << blockPagesShift,
			kHelAllocBacked, &block_bitmap_backing, &block_bitmap_frontal));
	HEL_CHECK(helCreateManagedMemory(numBlockGroups << blockPagesShift,
			kHelAllocBacked, &inode_bitmap_backing, &inode_bitmap_frontal));
	blockBitmap = helix::UniqueDescriptor{block_bitmap_frontal};
	inodeBitmap = helix::UniqueDescriptor{inode_bitmap_frontal};

	manageBlockBitmap(helix::UniqueDescriptor{block_bitmap_backing});
	manageInodeBitmap(helix::UniqueDescriptor{inode_bitmap_backing});

	// Create a memory bundle to manage the inode table.
	assert(!((inodesPerGroup * inodeSize) & 0xFFF));
	HelHandle inode_table_frontal;
	HelHandle inode_table_backing;
	HEL_CHECK(helCreateManagedMemory(inodesPerGroup * inodeSize * numBlockGroups,
			kHelAllocBacked, &inode_table_backing, &inode_table_frontal));
	inodeTable = helix::UniqueDescriptor{inode_table_frontal};

	manageInodeTable(helix::UniqueDescriptor{inode_table_backing});

	co_return;
}

async::detached FileSystem::manageBlockBitmap(helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		auto bg_idx = manage.offset() >> blockPagesShift;
		auto block = bgdt[bg_idx].blockBitmap;
		assert(block);

		assert(!(manage.offset() & ((1 << blockPagesShift) - 1))
				&& "TODO: propery support multi-page blocks");
		assert(manage.length() == (1 << blockPagesShift)
				&& "TODO: propery support multi-page blocks");

		if(manage.type() == kHelManageInitialize) {
			helix::Mapping bitmap_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->readSectors(block * sectorsPerBlock,
					bitmap_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
					manage.offset(), manage.length()));
		}else{
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping bitmap_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->writeSectors(block * sectorsPerBlock,
					bitmap_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
					manage.offset(), manage.length()));
		}
	}
}

async::detached FileSystem::manageInodeBitmap(helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		auto bg_idx = manage.offset() >> blockPagesShift;
		auto block = bgdt[bg_idx].inodeBitmap;
		assert(block);

		assert(!(manage.offset() & ((1 << blockPagesShift) - 1))
				&& "TODO: propery support multi-page blocks");
		assert(manage.length() == (1 << blockPagesShift)
				&& "TODO: propery support multi-page blocks");

		if(manage.type() == kHelManageInitialize) {
			helix::Mapping bitmap_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->readSectors(block * sectorsPerBlock,
					bitmap_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
					manage.offset(), manage.length()));
		}else{
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping bitmap_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->writeSectors(block * sectorsPerBlock,
					bitmap_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
					manage.offset(), manage.length()));
		}
	}
}

async::detached FileSystem::manageInodeTable(helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		// TODO: Make sure that we do not read/write past the end of the table.
		assert(!((inodesPerGroup * inodeSize) & (blockSize - 1)));

		// TODO: Use shifts instead of division.
		auto bg_idx = manage.offset() / (inodesPerGroup * inodeSize);
		auto bg_offset = manage.offset() % (inodesPerGroup * inodeSize);
		auto block = bgdt[bg_idx].inodeTable;
		assert(block);

		if(manage.type() == kHelManageInitialize) {
			helix::Mapping table_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->readSectors(block * sectorsPerBlock + bg_offset / 512,
					table_map.get(), manage.length() / 512);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
					manage.offset(), manage.length()));
		}else{
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping table_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->writeSectors(block * sectorsPerBlock + bg_offset / 512,
					table_map.get(), manage.length() / 512);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
					manage.offset(), manage.length()));
		}
	}
}

auto FileSystem::accessRoot() -> std::shared_ptr<Inode> {
	return accessInode(EXT2_ROOT_INO);
}

auto FileSystem::accessInode(uint32_t number) -> std::shared_ptr<Inode> {
	assert(number > 0);
	std::weak_ptr<Inode> &inode_slot = activeInodes[number];
	std::shared_ptr<Inode> active_inode = inode_slot.lock();
	if(active_inode)
		return active_inode;

	auto new_inode = std::make_shared<Inode>(*this, number);
	inode_slot = std::weak_ptr<Inode>(new_inode);
	initiateInode(new_inode);

	return new_inode;
}

async::result<std::shared_ptr<Inode>> FileSystem::createRegular() {
	auto ino = co_await allocateInode();
	assert(ino);

	// Lock and map the inode table.
	auto inode_address = (ino - 1) * inodeSize;

	helix::LockMemoryView lock_inode;
	auto &&submit = helix::submitLockMemoryView(inodeTable,
			&lock_inode, inode_address & ~(pageSize - 1), pageSize,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_inode.error());

	helix::Mapping inode_map{inodeTable,
				inode_address, inodeSize,
				kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	// TODO: Set the UID, GID, timestamps.
	auto disk_inode = reinterpret_cast<DiskInode *>(inode_map.get());
	auto generation = disk_inode->generation;
	memset(disk_inode, 0, inodeSize);
	disk_inode->mode = EXT2_S_IFREG;
	disk_inode->generation = generation + 1;
	struct timespec time;
	// TODO: Move to CLOCK_REALTIME when supported
	clock_gettime(CLOCK_MONOTONIC, &time);
	disk_inode->atime = time.tv_sec;
	disk_inode->ctime = time.tv_sec;
	disk_inode->mtime = time.tv_sec;

	co_return accessInode(ino);
}

async::result<std::shared_ptr<Inode>> FileSystem::createDirectory() {
	auto ino = co_await allocateInode();
	assert(ino);

	// Lock and map the inode table.
	auto inode_address = (ino - 1) * inodeSize;

	helix::LockMemoryView lock_inode;
	auto &&submit = helix::submitLockMemoryView(inodeTable,
			&lock_inode, inode_address & ~(pageSize - 1), pageSize,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_inode.error());

	helix::Mapping inode_map{inodeTable,
				inode_address, inodeSize,
				kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	// TODO: Set the UID, GID, timestamps.
	auto disk_inode = reinterpret_cast<DiskInode *>(inode_map.get());
	auto generation = disk_inode->generation;
	memset(disk_inode, 0, inodeSize);
	disk_inode->mode = EXT2_S_IFDIR;
	disk_inode->generation = generation + 1;
	struct timespec time;
	// TODO: Move to CLOCK_REALTIME when supported
	clock_gettime(CLOCK_MONOTONIC, &time);
	disk_inode->atime = time.tv_sec;
	disk_inode->ctime = time.tv_sec;
	disk_inode->mtime = time.tv_sec;

	// update usedDirsCount in the respective bgdt for this inode
	auto bg_idx = (ino - 1) / inodesPerGroup;
	bgdt[bg_idx].usedDirsCount++;
	co_await writebackBgdt();

	co_return accessInode(ino);
}

async::result<std::shared_ptr<Inode>> FileSystem::createSymlink() {
	auto ino = co_await allocateInode();
	assert(ino);

	// Lock and map the inode table.
	auto inode_address = (ino - 1) * inodeSize;

	helix::LockMemoryView lock_inode;
	auto &&submit = helix::submitLockMemoryView(inodeTable,
			&lock_inode, inode_address & ~(pageSize - 1), pageSize,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_inode.error());

	helix::Mapping inode_map{inodeTable,
				inode_address, inodeSize,
				kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};

	// TODO: Set the UID, GID, timestamps.
	auto disk_inode = reinterpret_cast<DiskInode *>(inode_map.get());
	auto generation = disk_inode->generation;
	memset(disk_inode, 0, inodeSize);
	disk_inode->mode = EXT2_S_IFLNK;
	disk_inode->generation = generation + 1;
	struct timespec time;
	// TODO: Move to CLOCK_REALTIME when supported
	clock_gettime(CLOCK_MONOTONIC, &time);
	disk_inode->atime = time.tv_sec;
	disk_inode->ctime = time.tv_sec;
	disk_inode->mtime = time.tv_sec;

	co_return accessInode(ino);
}

async::result<void> FileSystem::write(Inode *inode, uint64_t offset,
		const void *buffer, size_t length) {
	co_await inode->readyJump.wait();

	// Make sure that data blocks are allocated.
	auto blockOffset = (offset & ~(blockSize - 1)) >> blockShift;
	auto blockCount = ((offset & (blockSize - 1)) + length + (blockSize - 1)) >> blockShift;
	co_await assignDataBlocks(inode, blockOffset, blockCount);

	// Resize the file if necessary.
	if(offset + length > inode->fileSize()) {
		HEL_CHECK(helResizeMemory(inode->backingMemory,
				(offset + length + 0xFFF) & ~size_t(0xFFF)));
		inode->setFileSize(offset + length);
		auto syncInode = co_await helix_ng::synchronizeSpace(
				helix::BorrowedDescriptor{kHelNullHandle},
				inode->diskMapping.get(), inodeSize);
		HEL_CHECK(syncInode.error());
	}

	// TODO: If we *know* that the pages are already available,
	//       we can also fall back to the following "old" mapping code.
/*
	auto mapOffset = offset & ~size_t(0xFFF);
	auto mapSize = ((offset & size_t(0xFFF)) + length + 0xFFF) & ~size_t(0xFFF);

	helix::LockMemoryView lockMemory;
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(inode->frontalMemory),
			&lockMemory, mapOffset, mapSize, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lockMemory.error());

	// Map the page cache into the address space.
	helix::Mapping fileMap{helix::BorrowedDescriptor{inode->frontalMemory},
			static_cast<ptrdiff_t>(mapOffset), mapSize,
			kHelMapProtWrite | kHelMapDontRequireBacking};

	memcpy(reinterpret_cast<char *>(fileMap.get()) + (offset - mapOffset),
			buffer, length);
*/

	auto writeMemory = co_await helix_ng::writeMemory(
			helix::BorrowedDescriptor(inode->frontalMemory),
			offset, length, buffer);
	HEL_CHECK(writeMemory.error());
}

async::detached FileSystem::initiateInode(std::shared_ptr<Inode> inode) {
	// TODO: Use a shift instead of a division.
	auto inode_address = (inode->number - 1) * inodeSize;

	helix::LockMemoryView lock_inode;
	auto &&submit = helix::submitLockMemoryView(inodeTable,
			&lock_inode, inode_address & ~(pageSize - 1), pageSize,
			helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_inode.error());
	inode->diskLock = lock_inode.descriptor();

	inode->diskMapping = helix::Mapping{inodeTable,
			inode_address, inodeSize,
			kHelMapProtWrite | kHelMapProtRead | kHelMapDontRequireBacking};
	auto disk_inode = inode->diskInode();
//	printf("Inode %u: file size: %u\n", inode->number, disk_inode.size);

	if((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG) {
		inode->fileType = kTypeRegular;
	}else if((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
		inode->fileType = kTypeSymlink;
	}else if((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR) {
		inode->fileType = kTypeDirectory;
	}else{
		std::cerr << "ext2fs: Unexpected inode type " << (disk_inode->mode & EXT2_S_IFMT)
				<< " for inode " << inode->number << std::endl;
		abort();
	}

	// TODO: support large uid / gids
	inode->uid = disk_inode->uid;
	inode->gid = disk_inode->gid;

	// Allocate a page cache for the file.
	auto cache_size = (inode->fileSize() + 0xFFF) & ~size_t(0xFFF);
	HEL_CHECK(helCreateManagedMemory(cache_size, kHelAllocBacked,
			&inode->backingMemory, &inode->frontalMemory));

	if (inode->fileType == kTypeDirectory) {
		auto mapSize = (inode->fileSize() + 0xFFF) & ~size_t(0xFFF);
		inode->fileMapping = helix::Mapping{helix::BorrowedDescriptor{inode->frontalMemory},
				0, mapSize,
				kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};
	}

	HelHandle frontalOrder1, frontalOrder2;
	HelHandle backingOrder1, backingOrder2;
	HEL_CHECK(helCreateManagedMemory(3 << blockPagesShift,
			kHelAllocBacked, &backingOrder1, &frontalOrder1));
	HEL_CHECK(helCreateManagedMemory((blockSize / 4) << blockPagesShift,
			kHelAllocBacked, &backingOrder2, &frontalOrder2));
	inode->indirectOrder1 = helix::UniqueDescriptor{frontalOrder1};
	inode->indirectOrder2 = helix::UniqueDescriptor{frontalOrder2};

	manageIndirect(inode, 1, helix::UniqueDescriptor{backingOrder1});
	manageIndirect(inode, 2, helix::UniqueDescriptor{backingOrder2});
	manageFileData(inode);

	inode->isReady = true;
	inode->readyJump.raise();
}

async::detached FileSystem::manageFileData(std::shared_ptr<Inode> inode) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit = helix::submitManageMemory(helix::BorrowedDescriptor(inode->backingMemory),
				&manage, helix::Dispatcher::global());
		co_await submit.async_wait();
		HEL_CHECK(manage.error());
		assert(manage.offset() + manage.length() <= ((inode->fileSize() + 0xFFF) & ~size_t(0xFFF)));

		if(manage.type() == kHelManageInitialize) {
			helix::Mapping file_map{helix::BorrowedDescriptor{inode->backingMemory},
					static_cast<ptrdiff_t>(manage.offset()), manage.length(), kHelMapProtWrite};

			assert(!(manage.offset() % inode->fs.blockSize));
			size_t backed_size = std::min(manage.length(), inode->fileSize() - manage.offset());
			size_t num_blocks = (backed_size + (inode->fs.blockSize - 1)) / inode->fs.blockSize;

			assert(num_blocks * inode->fs.blockSize <= manage.length());
			co_await inode->fs.readDataBlocks(inode, manage.offset() / inode->fs.blockSize,
					num_blocks, file_map.get());

			HEL_CHECK(helUpdateMemory(inode->backingMemory, kHelManageInitialize,
					manage.offset(), manage.length()));
		}else{
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping file_map{helix::BorrowedDescriptor{inode->backingMemory},
					static_cast<ptrdiff_t>(manage.offset()), manage.length(), kHelMapProtRead};

			assert(!(manage.offset() % inode->fs.blockSize));
			size_t backed_size = std::min(manage.length(), inode->fileSize() - manage.offset());
			size_t num_blocks = (backed_size + (inode->fs.blockSize - 1)) / inode->fs.blockSize;

			assert(num_blocks * inode->fs.blockSize <= manage.length());
			co_await inode->fs.writeDataBlocks(inode, manage.offset() / inode->fs.blockSize,
					num_blocks, file_map.get());

			HEL_CHECK(helUpdateMemory(inode->backingMemory, kHelManageWriteback,
					manage.offset(), manage.length()));
		}
	}
}

async::detached FileSystem::manageIndirect(std::shared_ptr<Inode> inode,
		int order, helix::UniqueDescriptor memory) {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit_manage = helix::submitManageMemory(memory,
				&manage, helix::Dispatcher::global());
		co_await submit_manage.async_wait();
		HEL_CHECK(manage.error());

		uint32_t element = manage.offset() >> blockPagesShift;

		uint32_t block;
		if(order == 1) {
			auto disk_inode = inode->diskInode();

			switch(element) {
			case 0: block = disk_inode->data.blocks.singleIndirect; break;
			case 1: block = disk_inode->data.blocks.doubleIndirect; break;
			case 2: block = disk_inode->data.blocks.tripleIndirect; break;
			default:
				assert(!"unexpected offset");
				abort();
			}
		}else{
			assert(order == 2);

			auto indirect_frame = element >> (blockShift - 2);
			auto indirect_index = element & ((1 << (blockShift - 2)) - 1);

			helix::LockMemoryView lock_indirect;
			auto &&submit_indirect = helix::submitLockMemoryView(inode->indirectOrder1,
					&lock_indirect,
					(1 + indirect_frame) << blockPagesShift, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit_indirect.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder1,
					(1 + indirect_frame) << blockPagesShift, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};
			block = reinterpret_cast<uint32_t *>(indirect_map.get())[indirect_index];
		}

		assert(!(manage.offset() & ((1 << blockPagesShift) - 1))
				&& "TODO: propery support multi-page blocks");
		assert(manage.length() == (1 << blockPagesShift)
				&& "TODO: propery support multi-page blocks");

		if (manage.type() == kHelManageInitialize) {
			helix::Mapping out_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->readSectors(block * sectorsPerBlock,
					out_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageInitialize,
					manage.offset(), manage.length()));
		} else {
			assert(manage.type() == kHelManageWriteback);

			helix::Mapping out_map{memory,
					static_cast<ptrdiff_t>(manage.offset()), manage.length()};
			co_await device->writeSectors(block * sectorsPerBlock,
					out_map.get(), sectorsPerBlock);
			HEL_CHECK(helUpdateMemory(memory.getHandle(), kHelManageWriteback,
					manage.offset(), manage.length()));

		}
	}
}

async::result<uint32_t> FileSystem::allocateBlock() {
	for(uint32_t bg_idx = 0; bg_idx < numBlockGroups; bg_idx++) {
		helix::LockMemoryView lock_bitmap;
		auto &&submit_bitmap = helix::submitLockMemoryView(blockBitmap,
				&lock_bitmap,
				bg_idx << blockPagesShift, 1 << blockPagesShift,
				helix::Dispatcher::global());
		co_await submit_bitmap.async_wait();
		HEL_CHECK(lock_bitmap.error());

		helix::Mapping bitmap_map{blockBitmap,
				bg_idx << blockPagesShift, size_t{1} << blockPagesShift,
				kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

		// TODO: Update the block group descriptor table.

		auto words = reinterpret_cast<uint32_t *>(bitmap_map.get());
		for(unsigned int i = 0; i < (blocksPerGroup + 31) / 32; i++) {
			if(words[i] == 0xFFFFFFFF)
				continue;
			for(int j = 0; j < 32; j++) {
				if(words[i] & (static_cast<uint32_t>(1) << j))
					continue;
				// TODO: Make sure we never return reserved blocks.
				// TODO: Make sure we never return blocks higher than the max. block in the SB.
				auto block = bg_idx * blocksPerGroup + i * 32 + j;
				assert(block);
				assert(block < blocksCount);
				words[i] |= static_cast<uint32_t>(1) << j;

				bgdt[bg_idx].freeBlocksCount--;
				co_await writebackBgdt();

				co_return block;
			}
			assert(!"Failed to find zero-bit");
		}
	}

	co_return 0;
}

async::result<uint32_t> FileSystem::allocateInode() {
	// TODO: Do not start at block group zero.
	for(uint32_t bg_idx = 0; bg_idx < numBlockGroups; bg_idx++) {
		helix::LockMemoryView lock_bitmap;
		auto &&submit_bitmap = helix::submitLockMemoryView(inodeBitmap,
				&lock_bitmap,
				bg_idx << blockPagesShift, 1 << blockPagesShift,
				helix::Dispatcher::global());
		co_await submit_bitmap.async_wait();
		HEL_CHECK(lock_bitmap.error());

		helix::Mapping bitmap_map{inodeBitmap,
				bg_idx << blockPagesShift, size_t{1} << blockPagesShift,
				kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};

		// TODO: Update the block group descriptor table.

		auto words = reinterpret_cast<uint32_t *>(bitmap_map.get());
		for(unsigned int i = 0; i < (inodesPerGroup + 31) / 32; i++) {
			if(words[i] == 0xFFFFFFFF)
				continue;
			for(int j = 0; j < 32; j++) {
				if(words[i] & (static_cast<uint32_t>(1) << j))
					continue;
				// TODO: Make sure we never return reserved inodes.
				// TODO: Make sure we never return inodes higher than the max. inode in the SB.
				auto ino = bg_idx * inodesPerGroup + i * 32 + j + 1;
				assert(ino);
				assert(ino < inodesCount);
				words[i] |= static_cast<uint32_t>(1) << j;

				bgdt[bg_idx].freeInodesCount--;
				co_await writebackBgdt();

				co_return ino;
			}
			assert(!"Failed to find zero-bit");
		}
	}

	co_return 0;
}

async::result<void> FileSystem::assignDataBlocks(Inode *inode,
		uint64_t block_offset, size_t num_blocks) {
	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	auto disk_inode = inode->diskInode();

	size_t prg = 0;
	while(prg < num_blocks) {
		if(block_offset + prg < i_range) {
			while(prg < num_blocks
					&& block_offset + prg < i_range) {
				auto idx = block_offset + prg;
				if(disk_inode->data.blocks.direct[idx]) {
					prg++;
					continue;
				}
				auto block = co_await allocateBlock();
				assert(block && "Out of disk space"); // TODO: Fix this.
				disk_inode->blocks += (blockSize / 512);
				disk_inode->data.blocks.direct[idx] = block;
				prg++;
			}
		}else if(block_offset + prg < s_range) {
			bool needsReset = false;

			// Allocate the single-indirect block itself.
			if(!disk_inode->data.blocks.singleIndirect) {
				auto block = co_await allocateBlock();
				assert(block && "Out of disk space"); // TODO: Fix this.
				disk_inode->blocks += (blockSize / 512);
				disk_inode->data.blocks.singleIndirect = block;
				needsReset = true;
			}

			helix::LockMemoryView lock_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder1,
					&lock_indirect, 0, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder1,
					0, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapProtWrite | kHelMapDontRequireBacking};
			auto window = reinterpret_cast<uint32_t *>(indirect_map.get());

			if(needsReset)
				memset(window, 0, size_t{1} << blockPagesShift);

			while(prg < num_blocks
					&& block_offset + prg < s_range) {
				auto idx = block_offset + prg - i_range;
				if(window[idx]) {
					prg++;
					continue;
				}
				auto block = co_await allocateBlock();
				assert(block && "Out of disk space"); // TODO: Fix this.
				disk_inode->blocks += (blockSize / 512);
				window[idx] = block;
				prg++;
			}
		}else if(block_offset + prg < d_range) {
			assert(!"TODO: Implement allocation in double indirect blocks");
		}else{
			assert(!"TODO: Implement allocation in triple indirect blocks");
		}
	}

	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			inode->diskMapping.get(), inodeSize);
	HEL_CHECK(syncInode.error());
}

async::result<void> FileSystem::readDataBlocks(std::shared_ptr<Inode> inode,
		uint64_t offset, size_t num_blocks, void *buffer) {
	// We perform "block-fusion" here i.e. we try to read/write multiple
	// consecutive blocks in a single read/writeSectors() operation.
	auto fuse = [] (size_t remaining, uint32_t *list, size_t limit) {
		size_t n = 1;
		while(n < remaining && n < limit) {
			if(list[n] != list[0] + n)
				break;
			n++;
		}
		return std::pair<size_t, size_t>{list[0], n};
	};

	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	co_await inode->readyJump.wait();
	// TODO: Assert that we do not read past the EOF.

	constexpr size_t indirectBufferSize = 8;

	std::array<uint32_t, indirectBufferSize> indirectBuffer;

	size_t progress = 0;
	while(progress < num_blocks) {
		// Block number and block count of the readSectors() command that we will issue here.
		std::pair<size_t, size_t> issue;

		auto index = offset + progress;
//		std::cout << "Reading " << index << "-th block from inode " << inode->number
//				<< " (" << progress << "/" << num_blocks << " in request)" << std::endl;

		assert(index < d_range);
		if(index >= d_range) {
			assert(!"Fix triple indirect blocks");
		}else if(index >= s_range) { // Use the double indirect block.
			auto remaining = num_blocks - progress;
			int64_t indirect_frame = (index - s_range) >> (blockShift - 2);
			int64_t indirect_index = (index - s_range) & ((1 << (blockShift - 2)) - 1);

			if (remaining > indirectBufferSize) {
				helix::LockMemoryView lock_indirect;
				auto &&submit = helix::submitLockMemoryView(inode->indirectOrder2, &lock_indirect,
						indirect_frame << blockPagesShift, 1 << blockPagesShift,
						helix::Dispatcher::global());
				co_await submit.async_wait();
				HEL_CHECK(lock_indirect.error());

				helix::Mapping indirect_map{inode->indirectOrder2,
						indirect_frame << blockPagesShift, size_t{1} << blockPagesShift,
						kHelMapProtRead | kHelMapDontRequireBacking};

				issue = fuse(num_blocks - progress,
						reinterpret_cast<uint32_t *>(indirect_map.get()) + indirect_index,
						per_indirect - indirect_index);
			} else {
				auto readMemory = co_await helix_ng::readMemory(
						helix::BorrowedDescriptor{inode->indirectOrder2},
						(indirect_frame << blockPagesShift) + indirect_index * 4,
						remaining * 4, indirectBuffer.data());
				HEL_CHECK(readMemory.error());

				issue = fuse(remaining, indirectBuffer.data(), remaining);
			}
		}else if(index >= i_range) { // Use the single indirect block.
			auto remaining = num_blocks - progress;

			if (remaining > indirectBufferSize) {
				helix::LockMemoryView lock_indirect;
				auto &&submit = helix::submitLockMemoryView(inode->indirectOrder1,
						&lock_indirect, 0, 1 << blockPagesShift,
						helix::Dispatcher::global());
				co_await submit.async_wait();
				HEL_CHECK(lock_indirect.error());

				helix::Mapping indirect_map{inode->indirectOrder1,
						0, size_t{1} << blockPagesShift,
						kHelMapProtRead | kHelMapDontRequireBacking};

				auto indirect_index = index - i_range;
				issue = fuse(remaining,
						reinterpret_cast<uint32_t *>(indirect_map.get()) + indirect_index,
						per_indirect - indirect_index);
			} else {
				auto indirect_index = index - i_range;
				auto readMemory = co_await helix_ng::readMemory(
						helix::BorrowedDescriptor{inode->indirectOrder1},
						indirect_index * 4, remaining * 4, indirectBuffer.data());
				HEL_CHECK(readMemory.error());

				issue = fuse(remaining, indirectBuffer.data(), remaining);
			}
		}else{
			auto disk_inode = inode->diskInode();

			issue = fuse(num_blocks - progress,
					disk_inode->data.blocks.direct + index, 12 - index);
		}

//		std::cout << "Issuing read of " << issue.second
//				<< " blocks, starting at " << issue.first << std::endl;

		assert(issue.first);
		co_await device->readSectors(issue.first * sectorsPerBlock,
				(uint8_t *)buffer + progress * blockSize,
				issue.second * sectorsPerBlock);
		progress += issue.second;
	}
}

// TODO: There is a lot of overlap between this method and readDataBlocks.
//       Refactor common code into a another method.
async::result<void> FileSystem::writeDataBlocks(std::shared_ptr<Inode> inode,
		uint64_t offset, size_t num_blocks, const void *buffer) {
	// We perform "block-fusion" here i.e. we try to read/write multiple
	// consecutive blocks in a single read/writeSectors() operation.
	auto fuse = [] (size_t index, size_t remaining, uint32_t *list, size_t limit) {
		size_t n = 1;
		while(n < remaining && index + n < limit) {
			if(list[index + n] != list[index] + n)
				break;
			n++;
		}
		return std::pair<size_t, size_t>{list[index], n};
	};

	size_t per_indirect = blockSize / 4;
	size_t per_single = per_indirect;
	size_t per_double = per_indirect * per_indirect;

	// Number of blocks that can be accessed by:
	size_t i_range = 12; // Direct blocks only.
	size_t s_range = i_range + per_single; // Plus the first single indirect block.
	size_t d_range = s_range + per_double; // Plus the first double indirect block.

	co_await inode->readyJump.wait();
	// TODO: Assert that we do not write past the EOF.

	size_t progress = 0;
	while(progress < num_blocks) {
		// Block number and block count of the writeSectors() command that we will issue here.
		std::pair<size_t, size_t> issue;

		auto index = offset + progress;
//		std::cout << "Write " << index << "-th block to inode " << inode->number
//				<< " (" << progress << "/" << num_blocks << " in request)" << std::endl;

		assert(index < d_range);
		if(index >= d_range) {
			assert(!"Fix triple indirect blocks");
		}else if(index >= s_range) { // Use the double indirect block.
			// TODO: Use shift/and instead of div/mod.
			int64_t indirect_frame = (index - s_range) >> (blockShift - 2);
			int64_t indirect_index = (index - s_range) & ((1 << (blockShift - 2)) - 1);

			helix::LockMemoryView lock_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder2, &lock_indirect,
					indirect_frame << blockPagesShift, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder2,
					indirect_frame << blockPagesShift, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};

			issue = fuse(indirect_index, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirect_map.get()), per_indirect);
		}else if(index >= i_range) { // Use the triple indirect block.
			helix::LockMemoryView lock_indirect;
			auto &&submit = helix::submitLockMemoryView(inode->indirectOrder1,
					&lock_indirect, 0, 1 << blockPagesShift,
					helix::Dispatcher::global());
			co_await submit.async_wait();
			HEL_CHECK(lock_indirect.error());

			helix::Mapping indirect_map{inode->indirectOrder1,
					0, size_t{1} << blockPagesShift,
					kHelMapProtRead | kHelMapDontRequireBacking};
			issue = fuse(index - i_range, num_blocks - progress,
					reinterpret_cast<uint32_t *>(indirect_map.get()), per_indirect);
		}else{
			auto disk_inode = inode->diskInode();

			issue = fuse(index, num_blocks - progress,
					disk_inode->data.blocks.direct, 12);
		}

//		std::cout << "Issuing write of " << issue.second
//				<< " blocks, starting at " << issue.first << std::endl;

		assert(issue.first);
		co_await device->writeSectors(issue.first * sectorsPerBlock,
				(const uint8_t *)buffer + progress * blockSize,
				issue.second * sectorsPerBlock);
		progress += issue.second;
	}
}


async::result<void> FileSystem::truncate(Inode *inode, size_t size) {
	HEL_CHECK(helResizeMemory(inode->backingMemory,
			(size + 0xFFF) & ~size_t(0xFFF)));
	inode->setFileSize(size);
	auto syncInode = co_await helix_ng::synchronizeSpace(
			helix::BorrowedDescriptor{kHelNullHandle},
			inode->diskMapping.get(), inodeSize);
	HEL_CHECK(syncInode.error());
	co_return;
}

async::result<void> FileSystem::writebackBgdt() {
	auto bgdt_offset = (2048 + blockSize - 1) & ~size_t(blockSize - 1);
	co_await device->writeSectors((bgdt_offset >> blockShift) * sectorsPerBlock,
			blockGroupDescriptorBuffer.data(), blockGroupDescriptorBuffer.size() / 512);
}

// --------------------------------------------------------
// OpenFile
// --------------------------------------------------------

OpenFile::OpenFile(std::shared_ptr<Inode> inode)
: inode(inode), offset(0) { }

async::result<std::optional<std::string>>
OpenFile::readEntries() {
	co_await inode->readyJump.wait();

	if (inode->fileType != kTypeDirectory) {
		std::cout << "\e[33m" "ext2fs: readEntries called on something that's not a directory\e[39m" << std::endl;
		co_return std::nullopt; // FIXME: this does not indicate an error
	}

	auto map_size = (inode->fileSize() + 0xFFF) & ~size_t(0xFFF);

	helix::LockMemoryView lock_memory;
	auto &&submit = helix::submitLockMemoryView(helix::BorrowedDescriptor(inode->frontalMemory),
			&lock_memory, 0, map_size, helix::Dispatcher::global());
	co_await submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// Map the page cache into the address space.
	helix::Mapping file_map{helix::BorrowedDescriptor{inode->frontalMemory},
			0, map_size,
			kHelMapProtRead | kHelMapDontRequireBacking};

	// Read the directory structure.
	assert(offset <= inode->fileSize());
	while(offset < inode->fileSize()) {
		assert(!(offset & 3));
		assert(offset + sizeof(DiskDirEntry) <= inode->fileSize());
		auto disk_entry = reinterpret_cast<DiskDirEntry *>(
				reinterpret_cast<char *>(file_map.get()) + offset);
		assert(offset + disk_entry->recordLength <= inode->fileSize());

		offset += disk_entry->recordLength;

		if(disk_entry->inode) {
		//	std::cout << "libblockfs: Returning entry "
		//			<< std::string(disk_entry->name, disk_entry->nameLength) << std::endl;
			co_return std::string(disk_entry->name, disk_entry->nameLength);
		}
	}
	assert(offset == inode->fileSize());

	co_return std::nullopt;
}

} } // namespace blockfs::ext2fs

