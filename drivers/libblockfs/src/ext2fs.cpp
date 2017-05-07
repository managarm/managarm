
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>

#include <cofiber.hpp>
#include <helix/ipc.hpp>
#include <helix/await.hpp>

#include "ext2fs.hpp"

namespace blockfs {
namespace ext2fs {

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

Inode::Inode(FileSystem &fs, uint32_t number)
: fs(fs), number(number), isReady(false) { }

COFIBER_ROUTINE(async::result<std::experimental::optional<DirEntry>>,
		Inode::findEntry(std::string name), ([=] {
	assert(!name.empty() && name != "." && name != "..");

	COFIBER_AWAIT readyJump.async_wait();

	auto map_size = fileSize;
	if(map_size % 0x1000 != 0)
		map_size += 0x1000 - map_size % 0x1000;

	helix::LockMemory lock_memory;
	auto &&submit = helix::submitLockMemory(helix::BorrowedDescriptor(frontalMemory), &lock_memory,
			0, map_size, helix::Dispatcher::global());
	COFIBER_AWAIT submit.async_wait();
	HEL_CHECK(lock_memory.error());

	// TODO: Use a RAII mapping class to get rid of the mapping on return.
	// map the page cache into the address space
	void *window;
	HEL_CHECK(helMapMemory(frontalMemory, kHelNullHandle,
			nullptr, 0, map_size, kHelMapReadWrite | kHelMapDontRequireBacking, &window));

	// read the directory structure
	uintptr_t offset = 0;
	while(offset < fileSize) {
		auto disk_entry = reinterpret_cast<DiskDirEntry *>((char *)window + offset);
		// TODO: use memcmp?
		if(name.length() == disk_entry->nameLength
				&& strncmp(disk_entry->name, name.c_str(), disk_entry->nameLength) == 0) {
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

			COFIBER_RETURN(entry);
		}

		offset += disk_entry->recordLength;
	}
	assert(offset == fileSize);

	COFIBER_RETURN(std::experimental::nullopt);
}))

// --------------------------------------------------------
// FileSystem
// --------------------------------------------------------

FileSystem::FileSystem(BlockDevice *device)
: device(device), blockCache(*this) {
	blockCache.preallocate(32);
}

COFIBER_ROUTINE(async::result<void>, FileSystem::init(), ([=] {
	// TODO: Use std::string instead of malloc().
	auto superblock_buffer = malloc(1024);
	
	COFIBER_AWAIT device->readSectors(2, superblock_buffer, 2);
	
	DiskSuperblock *sb = (DiskSuperblock *)superblock_buffer;
	assert(sb->magic == 0xEF53);

	inodeSize = sb->inodeSize;
	blockSize = 1024 << sb->logBlockSize;
	sectorsPerBlock = blockSize / 512;
	numBlockGroups = sb->blocksCount / sb->blocksPerGroup;
	inodesPerGroup = sb->inodesPerGroup;

	size_t bgdt_size = numBlockGroups * sizeof(DiskGroupDesc);
	if(bgdt_size % 512)
		bgdt_size += 512 - (bgdt_size % 512);
	// TODO: Use std::string instead of malloc().
	blockGroupDescriptorBuffer = malloc(bgdt_size);

	COFIBER_AWAIT device->readSectors(2 * sectorsPerBlock,
			blockGroupDescriptorBuffer, bgdt_size / 512);
	
	COFIBER_RETURN();
}))

auto FileSystem::accessRoot() -> std::shared_ptr<Inode> {
	return accessInode(EXT2_ROOT_INO);
}

auto FileSystem::accessInode(uint32_t number) -> std::shared_ptr<Inode> {
	assert(number > 0);
	std::weak_ptr<Inode> &inode_slot = activeInodes[number];
	std::shared_ptr<Inode> active_inode = inode_slot.lock();
	if(active_inode)
		return std::move(active_inode);
	
	auto new_inode = std::make_shared<Inode>(*this, number);
	inode_slot = std::weak_ptr<Inode>(new_inode);
	initiateInode(new_inode);

	return std::move(new_inode);
}

COFIBER_ROUTINE(cofiber::no_future, FileSystem::initiateInode(std::shared_ptr<Inode> inode),
		([=] {
	// TODO: Use std::string instead of malloc().
	auto sector_buffer = (char *)malloc(512);
	
	uint32_t block_group = (inode->number - 1) / inodesPerGroup;
	uint32_t index = (inode->number - 1) % inodesPerGroup;
	uint32_t offset = index * inodeSize;

	auto bgdt = (DiskGroupDesc *)blockGroupDescriptorBuffer;
	uint32_t inode_table_block = bgdt[block_group].inodeTable;

	uint32_t sector = inode_table_block * sectorsPerBlock + (offset / 512);
	COFIBER_AWAIT device->readSectors(sector, sector_buffer, 1);
	
	DiskInode *disk_inode = (DiskInode *)(sector_buffer + (offset % 512));
//	printf("Inode %u: file size: %u\n", inode->number, disk_inode->size);

	if((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFREG) {
		inode->fileType = kTypeRegular;
	}else if((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
		inode->fileType = kTypeSymlink;
	}else{
		assert((disk_inode->mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
		inode->fileType = kTypeDirectory;
	}

	// TODO: support large files
	inode->fileSize = disk_inode->size;
	inode->fileData = disk_inode->data;

	// filter out the file type from the mode
	// TODO: ext2fs stores a 32-bit mode
	inode->mode = disk_inode->mode & 0x0FFF;

	inode->numLinks = disk_inode->linksCount;
	// TODO: support large uid / gids
	inode->uid = disk_inode->uid;
	inode->gid = disk_inode->gid;
	inode->atime.tv_sec = disk_inode->atime;
	inode->atime.tv_nsec = 0;
	inode->mtime.tv_sec = disk_inode->mtime;
	inode->mtime.tv_nsec = 0;
	inode->ctime.tv_sec = disk_inode->ctime;
	inode->ctime.tv_nsec = 0;

	// allocate a page cache for the file
	size_t cache_size = inode->fileSize;
	if(cache_size % 0x1000)
		cache_size += 0x1000 - cache_size % 0x1000;
	HEL_CHECK(helCreateManagedMemory(cache_size, kHelAllocBacked,
			&inode->backingMemory, &inode->frontalMemory));
	
	inode->isReady = true;
	inode->readyJump.trigger();

	manageInode(inode);
}))

COFIBER_ROUTINE(cofiber::no_future, FileSystem::manageInode(std::shared_ptr<Inode> inode),
		([=] {
	while(true) {
		helix::ManageMemory manage;
		auto &&submit = helix::submitManageMemory(helix::BorrowedDescriptor(inode->backingMemory),
				&manage, helix::Dispatcher::global());
		COFIBER_AWAIT(submit.async_wait());
		HEL_CHECK(manage.error());
		
		void *window;
		HEL_CHECK(helMapMemory(inode->backingMemory, kHelNullHandle, nullptr,
				manage.offset(), manage.length(), kHelMapReadWrite, &window));

		assert(manage.offset() < inode->fileSize);
		size_t read_size = std::min(manage.length(), inode->fileSize - manage.offset());
		size_t num_blocks = read_size / inode->fs.blockSize;
		if(read_size % inode->fs.blockSize != 0)
			num_blocks++;

		assert(manage.offset() % inode->fs.blockSize == 0);
		COFIBER_AWAIT inode->fs.readData(inode, manage.offset() / inode->fs.blockSize,
				num_blocks, window);

		HEL_CHECK(helCompleteLoad(inode->backingMemory, manage.offset(), manage.length()));
		HEL_CHECK(helUnmapMemory(kHelNullHandle, window, manage.length()));
	}
}))

COFIBER_ROUTINE(async::result<void>, FileSystem::readData(std::shared_ptr<Inode> inode,
		uint64_t offset, size_t num_blocks, void *buffer), ([=] {
	// We perform "read-fusion" here i.e. we try to read multiple
	// consecutive blocks in a single readSectors() operation.
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

	COFIBER_AWAIT inode->readyJump.async_wait();
	// TODO: Assert that we do not read past the EOF.

	size_t progress = 0;
	while(progress < num_blocks) {
//		printf("Reading block %lu of inode %u\n", index, inode->number);
		BlockCache::Ref s_cache;
		BlockCache::Ref d_cache;

		// Block number and block count of the readSectors() command that we will issue here.
		std::pair<size_t, size_t> issue;

		auto index = offset + progress;
		assert(index < d_range);
		if(index >= d_range) {
			assert(!"Fix tripple indirect blocks");
		}else if(index >= s_range) {
			d_cache = blockCache.lock(inode->fileData.blocks.doubleIndirect);
			COFIBER_AWAIT d_cache->waitUntilReady();

			// TODO: Use shift/and instead of div/mod.
			auto d_element = (index - s_range) / per_single;
			auto s_element = (index - s_range) % per_single;
			s_cache = blockCache.lock(((uint32_t *)d_cache->buffer)[d_element]);
			COFIBER_AWAIT s_cache->waitUntilReady();
			issue = fuse(s_element, num_blocks - progress,
					(uint32_t *)s_cache->buffer, per_indirect);
		}else if(index >= i_range) {
			s_cache = blockCache.lock(inode->fileData.blocks.singleIndirect);
			COFIBER_AWAIT s_cache->waitUntilReady();
			issue = fuse(index - i_range, num_blocks - progress,
					(uint32_t *)s_cache->buffer, per_indirect);
		}else{
			issue = fuse(index, num_blocks - progress, inode->fileData.blocks.direct, 12);
		}

		assert(issue.first != 0);
		COFIBER_AWAIT device->readSectors(issue.first * sectorsPerBlock,
				(uint8_t *)buffer + progress * blockSize,
				issue.second * sectorsPerBlock);
		progress += issue.second;
	}

	COFIBER_RETURN();
}))

// --------------------------------------------------------
// FileSystem::BlockCacheEntry
// --------------------------------------------------------

COFIBER_ROUTINE(cofiber::no_future, FileSystem::BlockCacheEntry::initiate(FileSystem *fs,
		uint64_t block, BlockCacheEntry *entry), ([=] {
	assert(entry->state == BlockCacheEntry::kStateInitial);
	
	entry->state = BlockCacheEntry::kStateLoading;
	COFIBER_AWAIT fs->device->readSectors(block * fs->sectorsPerBlock,
			entry->buffer, fs->sectorsPerBlock);

	assert(entry->state == kStateLoading);
	entry->state = kStateReady;
	entry->readyJump.trigger();
}))

FileSystem::BlockCacheEntry::BlockCacheEntry(void *buffer)
: buffer(buffer), state(kStateInitial) { }

async::result<void> FileSystem::BlockCacheEntry::waitUntilReady() {
	assert(state == kStateLoading || state == kStateReady);
	return readyJump.async_wait();
}

// --------------------------------------------------------
// FileSystem::BlockCache
// --------------------------------------------------------

FileSystem::BlockCache::BlockCache(FileSystem &fs)
: fs(fs) { }

auto FileSystem::BlockCache::allocate() -> Element * {
	// TODO: Use std::string instead of malloc().
	void *buffer = malloc(fs.blockSize);
	return new Element{buffer};
}

void FileSystem::BlockCache::initEntry(uint64_t block, BlockCacheEntry *entry) {
	BlockCacheEntry::initiate(&fs, block, entry);
}

void FileSystem::BlockCache::finishEntry(BlockCacheEntry *entry) {
	assert(entry->state == BlockCacheEntry::kStateReady);
	entry->state = BlockCacheEntry::kStateInitial;
}


// --------------------------------------------------------
// OpenFile
// --------------------------------------------------------

OpenFile::OpenFile(std::shared_ptr<Inode> inode)
: inode(inode), offset(0) { }

// --------------------------------------------------------
// Client
// --------------------------------------------------------
/*
Client::Client(helx::EventHub &event_hub, FileSystem &fs)
: eventHub(event_hub), fs(fs), objectHandler(*this), mbusConnection(eventHub) {
	mbusConnection.setObjectHandler(&objectHandler);
}

void Client::init(frigg::CallbackPtr<void()> callback) {
	auto closure = new InitClosure(*this, callback);
	(*closure)();
}

// --------------------------------------------------------
// Client::ObjectHandler
// --------------------------------------------------------

Client::ObjectHandler::ObjectHandler(Client &client)
: client(client) { }

void Client::ObjectHandler::requireIf(bragi_mbus::ObjectId object_id,
		frigg::CallbackPtr<void(HelHandle)> callback) {
	helx::Pipe local, remote;
	helx::Pipe::createFullPipe(local, remote);
	callback(remote.getHandle());
	remote = helx::Pipe();

	auto closure = new Connection(client.eventHub, client.fs, std::move(local));
	(*closure)();
}

// --------------------------------------------------------
// Client::InitClosure
// --------------------------------------------------------

Client::InitClosure::InitClosure(Client &client, frigg::CallbackPtr<void()> callback)
: client(client), callback(callback) { }

void Client::InitClosure::operator() () {
	client.mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void Client::InitClosure::connected() {
	client.mbusConnection.registerObject("file-system",
			CALLBACK_MEMBER(this, &InitClosure::registered));
}

void Client::InitClosure::registered(bragi_mbus::ObjectId object_id) {
	callback();
}

// --------------------------------------------------------
// Connection
// --------------------------------------------------------

Connection::Connection(helx::EventHub &event_hub, FileSystem &fs, helx::Pipe pipe)
: eventHub(event_hub), fs(fs), pipe(std::move(pipe)), nextHandle(1) { }

void Connection::operator() () {
	HEL_CHECK(pipe.recvStringReq(buffer, 128, eventHub, kHelAnyRequest, 0,
			CALLBACK_MEMBER(this, &Connection::recvRequest)));
}

FileSystem &Connection::getFs() {
	return fs;
}

helx::Pipe &Connection::getPipe() {
	return pipe;
}

int Connection::attachOpenFile(OpenFile *file) {
	int handle = nextHandle++;
	fileHandles.insert(std::make_pair(handle, file));
	return handle;
}

OpenFile *Connection::getOpenFile(int handle) {
	return fileHandles.at(handle);
}

cofiber::no_future processStatRequest(Connection *connection, int64_t response_id,
			managarm::fs::CntRequest request);

cofiber::no_future processSeekRequest(Connection *connection, int64_t response_id,
			managarm::fs::CntRequest request);

cofiber::no_future processMapRequest(Connection *connection, int64_t response_id,
			managarm::fs::CntRequest request);

void Connection::recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::CntRequest request;
	request.ParseFromArray(buffer, length);

	if(request.req_type() == managarm::fs::CntReqType::FSTAT) {
		processStatRequest(this, msg_request, std::move(request));
	}else if(request.req_type() == managarm::fs::CntReqType::OPEN) {
		auto closure = new OpenClosure(*this, msg_request, std::move(request));
		(*closure)();
	}else if(request.req_type() == managarm::fs::CntReqType::READ) {
		auto closure = new ReadClosure(*this, msg_request, std::move(request));
		(*closure)();
	}else if(request.req_type() == managarm::fs::CntReqType::SEEK_ABS
			|| request.req_type() == managarm::fs::CntReqType::SEEK_REL
			|| request.req_type() == managarm::fs::CntReqType::SEEK_EOF) {
		processSeekRequest(this, msg_request, std::move(request));
	}else if(request.req_type() == managarm::fs::CntReqType::MMAP) {
		processMapRequest(this, msg_request, std::move(request));
	}else{
		fprintf(stderr, "Illegal request type\n");
		abort();
	}

	(*this)();
}

COFIBER_ROUTINE(cofiber::no_future, processStatRequest(Connection *connection, int64_t response_id,
		managarm::fs::CntRequest request), [=] () {
	// wait until the requested inode is ready
	auto open_file = connection->getOpenFile(request.fd());
	if(!open_file->inode->isReady)
		COFIBER_AWAIT WaitForInode(open_file->inode);

	managarm::fs::SvrResponse response;
	response.set_error(managarm::fs::Errors::SUCCESS);
	
	switch(open_file->inode->fileType) {
	case kTypeRegular:
		response.set_file_type(managarm::fs::FileType::REGULAR); break;
	case kTypeDirectory:
		response.set_file_type(managarm::fs::FileType::DIRECTORY); break;
	case kTypeSymlink:
		response.set_file_type(managarm::fs::FileType::SYMLINK); break;
	default:
		assert(!"Unexpected file type");
	}
	
	response.set_inode_num(open_file->inode->number);
	response.set_mode(open_file->inode->mode);
	response.set_num_links(open_file->inode->numLinks);
	response.set_uid(open_file->inode->uid);
	response.set_gid(open_file->inode->gid);
	response.set_file_size(open_file->inode->fileSize);

	response.set_atime_secs(open_file->inode->atime.tv_sec);
	response.set_atime_nanos(open_file->inode->atime.tv_nsec);
	response.set_mtime_secs(open_file->inode->mtime.tv_sec);
	response.set_mtime_nanos(open_file->inode->mtime.tv_nsec);
	response.set_ctime_secs(open_file->inode->ctime.tv_sec);
	response.set_ctime_nanos(open_file->inode->ctime.tv_nsec);

	std::string serialized;
	response.SerializeToString(&serialized);

	HelError resp_error = COFIBER_AWAIT connection->getPipe().sendStringResp(serialized.data(), serialized.size(),
			connection->getFs().eventHub, response_id, 0);
	HEL_CHECK(resp_error);
});

// --------------------------------------------------------
// OpenClosure
// --------------------------------------------------------

OpenClosure::OpenClosure(Connection &connection, int64_t response_id,
		managarm::fs::CntRequest request)
: connection(connection), responseId(response_id), request(std::move(request)) { }

void OpenClosure::operator() () {
	tailPath = request.path();
	directory = connection.getFs().accessRoot();

	if(tailPath.empty()) {
		auto action = libchain::compose([=] (std::string *serialized) {
			int handle = connection.attachOpenFile(new OpenFile(directory));

			managarm::fs::SvrResponse response;
			response.set_error(managarm::fs::Errors::SUCCESS);
			response.set_fd(handle);
			response.set_file_type(managarm::fs::FileType::DIRECTORY);

			response.SerializeToString(serialized);
			
			return connection.getPipe().sendStringResp(serialized->data(), serialized->size(),
					connection.getFs().eventHub, responseId, 0)
			+ libchain::lift([=] (HelError error) { 
				HEL_CHECK(error); 
				delete this;
			});
		}, std::string());
		libchain::run(std::move(action));
	}else{
		processSegment();
	}
}

void OpenClosure::processSegment() {
	assert(!tailPath.empty());

	size_t slash = tailPath.find('/');
	if(slash == std::string::npos) {
		directory->findEntry(tailPath,
				CALLBACK_MEMBER(this, &OpenClosure::foundEntry));
		tailPath.clear();
	}else{
		directory->findEntry(tailPath.substr(0, slash),
				CALLBACK_MEMBER(this, &OpenClosure::foundEntry));
		tailPath = tailPath.substr(slash + 1);
	}
}

void OpenClosure::foundEntry(std::experimental::optional<DirEntry> entry) {
	if(!entry) {
		auto action = libchain::compose([=] (std::string *serialized) {
			managarm::fs::SvrResponse response;
			response.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

			response.SerializeToString(serialized);
			
			return connection.getPipe().sendStringResp(serialized->data(), serialized->size(),
					connection.getFs().eventHub, responseId, 0)
			+ libchain::lift([=] (HelError error) {
				HEL_CHECK(error);
				delete this;
			});
		}, std::string());
		libchain::run(std::move(action));
		return;
	}
	
	auto inode = connection.getFs().accessInode(entry->inode);
	if(tailPath.empty()) {
		auto action = libchain::compose([=] (std::string *serialized) {
			int handle = connection.attachOpenFile(new OpenFile(inode));

			managarm::fs::SvrResponse response;
			response.set_error(managarm::fs::Errors::SUCCESS);
			response.set_fd(handle);

			switch(entry->fileType) {
			case kTypeRegular:
				response.set_file_type(managarm::fs::FileType::REGULAR); break;
			case kTypeSymlink:
				response.set_file_type(managarm::fs::FileType::SYMLINK); break;
			default:
				assert(!"Unexpected file type");
			}

			response.SerializeToString(serialized);
			
			return connection.getPipe().sendStringResp(serialized->data(),
					serialized->size(), connection.getFs().eventHub,
					responseId, 0)
			+ libchain::lift([=] (HelError error) { HEL_CHECK(error); });
		}, std::string());	
		libchain::run(std::move(action));
	}else{
		assert(entry->fileType == kTypeDirectory);
		directory = inode;
		processSegment();
	}
}

// --------------------------------------------------------
// ReadClosure
// --------------------------------------------------------

ReadClosure::ReadClosure(Connection &connection, int64_t response_id,
		managarm::fs::CntRequest request)
: connection(connection), responseId(response_id), request(std::move(request)) { }

void ReadClosure::operator() () {
	openFile = connection.getOpenFile(request.fd());
	if(openFile->inode->isReady) {
		inodeReady();
	}else{
		openFile->inode->readyQueue.push_back(CALLBACK_MEMBER(this, &ReadClosure::inodeReady));
	}
}

void ReadClosure::inodeReady() {
	if(openFile->offset >= openFile->inode->fileSize) {
		auto action = libchain::compose([=] (std::string *serialized) {
			managarm::fs::SvrResponse response;
			response.set_error(managarm::fs::Errors::END_OF_FILE);

			response.SerializeToString(serialized);
			
			return connection.getPipe().sendStringResp(serialized->data(), serialized->size(),
					connection.getFs().eventHub, responseId, 0)
			+ libchain::lift([=] (HelError error) { 
				HEL_CHECK(error); 
				delete this;
			});
		}, std::string());
		libchain::run(std::move(action));
		return;
	}
	
	if(openFile->inode->fileType == kTypeSymlink && openFile->inode->fileSize <= 60) {
		auto action = libchain::compose([=] (std::string *serialized) {
			size_t read_size = std::min({ size_t(request.size()),
					size_t(openFile->inode->fileSize - openFile->offset) });

			managarm::fs::SvrResponse response;
			response.set_error(managarm::fs::Errors::SUCCESS);

			response.SerializeToString(serialized);
			
			printf("[blockfs/src/ext2fs] sendStringResp OpenClosure:inodeReady2 \n");
			return connection.getPipe().sendStringResp(serialized->data(), serialized->size(),
					connection.getFs().eventHub, responseId, 0)
			+ libchain::lift([=] (HelError error) { HEL_CHECK(error); })
			+ connection.getPipe().sendStringResp(openFile->inode->fileData.embedded + openFile->offset,
					read_size, connection.getFs().eventHub, responseId, 1)
			+ libchain::lift([=] (HelError error) { 
				HEL_CHECK(error);
				openFile->offset += read_size;
				delete this;
			});
		}, std::string());
		libchain::run(std::move(action));
	}else{
		size_t read_size = std::min(size_t(request.size()),
				openFile->inode->fileSize - openFile->offset);
		assert(read_size > 0);
		
		// lock the requested region of the page cache
		size_t misalign = openFile->offset % 0x1000;
		size_t map_offset = openFile->offset - misalign;

		size_t map_size = read_size + misalign;
		if(map_size % 0x1000 != 0)
			map_size += 0x1000 - map_size % 0x1000;

		auto cb = CALLBACK_MEMBER(this, &ReadClosure::lockedMemory);
		int64_t async_id;
		HEL_CHECK(helSubmitLockMemory(openFile->inode->frontalMemory,
				connection.getFs().eventHub.getHandle(), map_offset, map_size,
				(uintptr_t)cb.getFunction(), (uintptr_t)cb.getObject(), &async_id));
	}
}

void ReadClosure::lockedMemory() {
	auto action = libchain::compose([=] (std::string *serialized) {
		size_t read_size = std::min(size_t(request.size()),
				openFile->inode->fileSize - openFile->offset);

		// map the page cache into memory
		size_t misalign = openFile->offset % 0x1000;
		size_t map_offset = openFile->offset - misalign;

		size_t map_size = read_size + misalign;
		if(map_size % 0x1000 != 0)
			map_size += 0x1000 - map_size % 0x1000;

		void *cache_ptr;
		HEL_CHECK(helMapMemory(openFile->inode->frontalMemory, kHelNullHandle, nullptr,
				map_offset, map_size, kHelMapReadWrite | kHelMapDontRequireBacking, &cache_ptr));

		// send cached data to the client
		managarm::fs::SvrResponse response;
		response.set_error(managarm::fs::Errors::SUCCESS);

		response.SerializeToString(serialized);
		
		return connection.getPipe().sendStringResp(serialized->data(), serialized->size(),
				connection.getFs().eventHub, responseId, 0)
		+ libchain::lift([=] (HelError error) { HEL_CHECK(error); })
		+ connection.getPipe().sendStringResp((char *)cache_ptr + misalign, read_size,
				connection.getFs().eventHub, responseId, 1)
		+ libchain::lift([=] (HelError error) { 
			HEL_CHECK(error); 
			openFile->offset += read_size;
			// unmap the page cache
			HEL_CHECK(helUnmapMemory(kHelNullHandle, cache_ptr, map_size));
			delete this;
		});
	}, std::string());
	libchain::run(std::move(action));
}

COFIBER_ROUTINE(cofiber::no_future, processSeekRequest(Connection *connection, int64_t response_id,
		managarm::fs::CntRequest request), [=] () {
	auto open_file = connection->getOpenFile(request.fd());
	assert(open_file->inode->isReady);

	if(request.req_type() == managarm::fs::CntReqType::SEEK_ABS) {
		open_file->offset = request.rel_offset();
	}else if(request.req_type() == managarm::fs::CntReqType::SEEK_REL) {
		open_file->offset += request.rel_offset();
	}else if(request.req_type() == managarm::fs::CntReqType::SEEK_EOF) {
		open_file->offset = open_file->inode->fileSize;
	}else{
		printf("Illegal SEEK request");
		abort();
	}
	
	managarm::fs::SvrResponse response;
	response.set_error(managarm::fs::Errors::SUCCESS);
	response.set_offset(open_file->offset);

	std::string serialized;
	response.SerializeToString(&serialized);
	
	HelError resp_error = COFIBER_AWAIT connection->getPipe().sendStringResp(serialized.data(), serialized.size(),
			connection->getFs().eventHub, response_id, 0);
	HEL_CHECK(resp_error);
});

COFIBER_ROUTINE(cofiber::no_future, processMapRequest(Connection *connection, int64_t response_id,
		managarm::fs::CntRequest request), [=] () {
	// wait until the requested inode is ready
	auto open_file = connection->getOpenFile(request.fd());
	if(!open_file->inode->isReady)
		COFIBER_AWAIT WaitForInode(open_file->inode);

	// send the response to the client
	managarm::fs::SvrResponse response;
	response.set_error(managarm::fs::Errors::SUCCESS);

	std::string serialized;
	response.SerializeToString(&serialized);
	
	HelError resp_error = COFIBER_AWAIT connection->getPipe().sendStringResp(serialized.data(),
			serialized.size(), connection->getFs().eventHub, response_id, 0);
	HEL_CHECK(resp_error);

	HelError data_error = COFIBER_AWAIT connection->getPipe().sendDescriptorResp(open_file->inode->frontalMemory,
				connection->getFs().eventHub, response_id, 1);
	HEL_CHECK(data_error);
});
*/

} } // namespace blockfs::ext2fs

