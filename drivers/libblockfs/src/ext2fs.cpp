
#include <stdlib.h>
#include <stdio.h>
#include <algorithm>
#include <libchain/all.hpp>
#include <cofiber.hpp>

#include "ext2fs.hpp"

namespace frigg {
	template<typename S, typename Functor>
	struct Awaiter {
	public:
		Awaiter(Await<S, Functor> chain)
		: _chain(std::move(chain)) { }

		bool await_ready() { return false; }
		HelError await_resume() { return _error; }

		void await_suspend(cofiber::coroutine_handle<> handle) {
			libchain::run(std::move(_chain), [=] (HelError error) {
				_error = error;
				handle.resume();
			});
		}
	private:
		Await<S, Functor> _chain;
		HelError _error;
	};
	
	template<typename S, typename Functor>
	Awaiter<S, Functor> cofiber_awaiter(Await<S, Functor> chain) {
		return Awaiter<S, Functor>(std::move(chain));
	}
};

namespace blockfs {
namespace ext2fs {

struct WaitForInode {
private:
public:
	friend auto cofiber_awaiter(WaitForInode action) {
		struct Awaiter {
			Awaiter(std::shared_ptr<Inode> inode)
			: _inode(std::move(inode)) { }

			bool await_ready() { return false; }
			void await_resume() { }

			void await_suspend(cofiber::coroutine_handle<> handle) {
				_handle = handle;
				_inode->readyQueue.push_back(CALLBACK_MEMBER(this, &Awaiter::onReady));
			}

		private:
			void onReady() {
				_handle.resume();
			}

			std::shared_ptr<Inode> _inode;
			cofiber::coroutine_handle<> _handle;
		};
	
		return Awaiter(std::move(action._inode));
	}

	WaitForInode(std::shared_ptr<Inode> inode)
	: _inode(std::move(inode)) { }

private:
	std::shared_ptr<Inode> _inode;
};

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

Inode::Inode(FileSystem &fs, uint32_t number)
: fs(fs), number(number), isReady(false) { }

struct FindEntryClosure {
	FindEntryClosure(std::shared_ptr<Inode> inode, std::string name,
			frigg::CallbackPtr<void(std::experimental::optional<DirEntry>)> callback);

	~FindEntryClosure();

	void operator() ();

	void inodeReady();
	void lockedMemory();

	std::shared_ptr<Inode> inode;
	std::string name;
	frigg::CallbackPtr<void(std::experimental::optional<DirEntry>)> callback;

	size_t mapSize;
	void *cachePtr;
};

FindEntryClosure::FindEntryClosure(std::shared_ptr<Inode> inode, std::string name,
		frigg::CallbackPtr<void(std::experimental::optional<DirEntry>)> callback)
: inode(std::move(inode)), name(name), callback(callback), cachePtr(nullptr) { }

FindEntryClosure::~FindEntryClosure() {
	// unmap the page cache
	if(cachePtr)
		HEL_CHECK(helUnmapMemory(kHelNullHandle, cachePtr, mapSize));
}

void FindEntryClosure::operator() () {
	if(inode->isReady) {
		inodeReady();
	}else{
		inode->readyQueue.push_back(CALLBACK_MEMBER(this, &FindEntryClosure::inodeReady));
	}
}

void FindEntryClosure::inodeReady() {
	mapSize = inode->fileSize;
	if(mapSize % 0x1000 != 0)
		mapSize += 0x1000 - mapSize % 0x1000;

	auto cb = CALLBACK_MEMBER(this, &FindEntryClosure::lockedMemory);
	int64_t async_id;
	HEL_CHECK(helSubmitLockMemory(inode->frontalMemory,
			inode->fs.eventHub.getHandle(), 0, mapSize,
			(uintptr_t)cb.getFunction(), (uintptr_t)cb.getObject(), &async_id));
}

void FindEntryClosure::lockedMemory() {
	// map the page cache into the address space
	HEL_CHECK(helMapMemory(inode->frontalMemory, kHelNullHandle,
		nullptr, 0, mapSize, kHelMapReadWrite | kHelMapDontRequireBacking, &cachePtr));

	// read the directory structure
	uintptr_t offset = 0;
	while(offset < inode->fileSize) {
		auto disk_entry = reinterpret_cast<DiskDirEntry *>((char *)cachePtr + offset);
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

			callback(entry);
			delete this;
			return;
		}

		offset += disk_entry->recordLength;
	}
	assert(offset == inode->fileSize);

	callback(std::experimental::nullopt);
	delete this;
}

void Inode::findEntry(std::string name,
		frigg::CallbackPtr<void(std::experimental::optional<DirEntry>)> callback) {
	assert(!name.empty() && name != "." && name != "..");

	auto closure = new FindEntryClosure(shared_from_this(), std::move(name), callback);
	(*closure)();
}

struct PageClosure {
	PageClosure(std::shared_ptr<Inode> inode, uintptr_t offset, size_t length);

	void operator() ();

	void readComplete();

	std::shared_ptr<Inode> inode;
	uintptr_t offset;
	size_t length;

	void *mapping;
};

PageClosure::PageClosure(std::shared_ptr<Inode> inode, uintptr_t offset, size_t length)
: inode(std::move(inode)), offset(offset), length(length) { }

void PageClosure::operator() () {
	HEL_CHECK(helMapMemory(inode->backingMemory, kHelNullHandle, nullptr,
			offset, length, kHelMapReadWrite, &mapping));

	assert(offset < inode->fileSize);
	size_t read_size = std::min(length, inode->fileSize - offset);
	size_t num_blocks = read_size / inode->fs.blockSize;
	if(read_size % inode->fs.blockSize != 0)
		num_blocks++;

	assert(offset % inode->fs.blockSize == 0);
	inode->fs.readData(inode, offset / inode->fs.blockSize, num_blocks, (char *)mapping,
			CALLBACK_MEMBER(this, &PageClosure::readComplete));
}

void PageClosure::readComplete() {
	HEL_CHECK(helCompleteLoad(inode->backingMemory, offset, length));

	HEL_CHECK(helUnmapMemory(kHelNullHandle, mapping, length));

	delete this;
}

void Inode::onLoadRequest(HelError error, uintptr_t offset, size_t length) {
	HEL_CHECK(error);

	auto closure = new PageClosure(shared_from_this(), offset, length);
	(*closure)();
	
	auto cb = CALLBACK_MEMBER(this, &Inode::onLoadRequest);
	int64_t async_id;
	HEL_CHECK(helSubmitProcessLoad(backingMemory, fs.eventHub.getHandle(),
			(uintptr_t)cb.getFunction(), (uintptr_t)cb.getObject(), &async_id));
}

// --------------------------------------------------------
// FileSystem
// --------------------------------------------------------

FileSystem::FileSystem(helx::EventHub &event_hub, BlockDevice *device)
: eventHub(event_hub), device(device), blockCache(*this) {
	blockCache.preallocate(32);
}

void FileSystem::init(frigg::CallbackPtr<void()> callback) {
	auto closure = new InitClosure(*this, callback);
	(*closure)();
}

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

	auto closure = new ReadInodeClosure(*this, new_inode);
	(*closure)();

	return std::move(new_inode);
}

void FileSystem::readData(std::shared_ptr<Inode> inode, uint64_t block_offset,
		size_t num_blocks, void *buffer, frigg::CallbackPtr<void()> callback) {
	auto closure = new ReadDataClosure(*this, std::move(inode), block_offset,
			num_blocks, buffer, callback);
	(*closure)();
}

// --------------------------------------------------------
// FileSystem::InitClosure
// --------------------------------------------------------

FileSystem::InitClosure::InitClosure(FileSystem &ext2fs, frigg::CallbackPtr<void()> callback)
: ext2fs(ext2fs), callback(callback) {
	superblockBuffer = (char *)malloc(1024);
}

void FileSystem::InitClosure::operator() () {
	ext2fs.device->readSectors(2, superblockBuffer, 2,
			CALLBACK_MEMBER(this, &InitClosure::readSuperblock));
}

void FileSystem::InitClosure::readSuperblock() {
	DiskSuperblock *sb = (DiskSuperblock *)superblockBuffer;
	assert(sb->magic == 0xEF53);

	ext2fs.inodeSize = sb->inodeSize;
	ext2fs.blockSize = 1024 << sb->logBlockSize;
	ext2fs.sectorsPerBlock = ext2fs.blockSize / 512;
	ext2fs.numBlockGroups = sb->blocksCount / sb->blocksPerGroup;
	ext2fs.inodesPerGroup = sb->inodesPerGroup;

	size_t bgdt_size = ext2fs.numBlockGroups * sizeof(DiskGroupDesc);
	if(bgdt_size % 512)
		bgdt_size += 512 - (bgdt_size % 512);
	ext2fs.blockGroupDescriptorBuffer = malloc(bgdt_size);

	ext2fs.device->readSectors(2 * ext2fs.sectorsPerBlock,
			ext2fs.blockGroupDescriptorBuffer, bgdt_size / 512,
			CALLBACK_MEMBER(this, &InitClosure::readBlockGroups));
}

void FileSystem::InitClosure::readBlockGroups() {
	callback();
	delete this;
};

// --------------------------------------------------------
// FileSystem::ReadInodeClosure
// --------------------------------------------------------

FileSystem::ReadInodeClosure::ReadInodeClosure(FileSystem &ext2fs, std::shared_ptr<Inode> inode)
: ext2fs(ext2fs), inode(std::move(inode)) {
	sectorBuffer = (char *)malloc(512);
}

void FileSystem::ReadInodeClosure::operator() () {
	uint32_t block_group = (inode->number - 1) / ext2fs.inodesPerGroup;
	uint32_t index = (inode->number - 1) % ext2fs.inodesPerGroup;

	auto bgdt = (DiskGroupDesc *)ext2fs.blockGroupDescriptorBuffer;
	uint32_t inode_table_block = bgdt[block_group].inodeTable;
	uint32_t offset = index * ext2fs.inodeSize;

	uint32_t sector = inode_table_block * ext2fs.sectorsPerBlock + (offset / 512);
	ext2fs.device->readSectors(sector, sectorBuffer, 1,
			CALLBACK_MEMBER(this, &ReadInodeClosure::readSector));
}

void FileSystem::ReadInodeClosure::readSector() {
	uint32_t index = (inode->number - 1) % ext2fs.inodesPerGroup;
	uint32_t offset = index * ext2fs.inodeSize;

	DiskInode *disk_inode = (DiskInode *)(sectorBuffer + (offset % 512));
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

	auto cb = CALLBACK_MEMBER(inode.get(), &Inode::onLoadRequest);
	int64_t async_id;
	HEL_CHECK(helSubmitProcessLoad(inode->backingMemory, ext2fs.eventHub.getHandle(),
			(uintptr_t)cb.getFunction(), (uintptr_t)cb.getObject(), &async_id));
	
	inode->isReady = true;
	for(auto it = inode->readyQueue.begin(); it != inode->readyQueue.end(); ++it)
		(*it)();
	inode->readyQueue.clear();

	delete this;
}

// --------------------------------------------------------
// FileSystem::ReadDataClosure
// --------------------------------------------------------

FileSystem::ReadDataClosure::ReadDataClosure(FileSystem &ext2fs, std::shared_ptr<Inode> inode,
		uint64_t block_offset, size_t num_blocks, void *buffer,
		frigg::CallbackPtr<void()> callback)
: ext2fs(ext2fs), inode(std::move(inode)), blockOffset(block_offset),
		numBlocks(num_blocks), buffer(buffer), callback(callback),
		blocksRead(0) { }

void FileSystem::ReadDataClosure::operator() () {
	if(inode->isReady) {
		inodeReady();
	}else{
		inode->readyQueue.push_back(CALLBACK_MEMBER(this, &ReadDataClosure::inodeReady));
	}
}

void FileSystem::ReadDataClosure::inodeReady() {
	if(blocksRead < numBlocks) {
		size_t block_index = blockOffset + blocksRead;
//		printf("Reading block %lu of inode %u\n", block_index, inode->number);

		size_t per_single = ext2fs.blockSize / 4;
		size_t per_double = per_single * per_single;

		size_t single_offset = 12;
		size_t double_offset = single_offset + per_single;
		size_t triple_offset = double_offset + per_double;

		if(block_index < single_offset) {
			uint32_t block = inode->fileData.blocks.direct[block_index];
			assert(block != 0);

			chunkSize = 1;
			while(block_index + chunkSize < single_offset && blocksRead + chunkSize < numBlocks) {
				if(inode->fileData.blocks.direct[block_index + chunkSize] != block + chunkSize)
					break;
				chunkSize++;
			}

			ext2fs.device->readSectors(block * ext2fs.sectorsPerBlock,
					(uint8_t *)buffer + blocksRead * ext2fs.blockSize,
					chunkSize * ext2fs.sectorsPerBlock,
					CALLBACK_MEMBER(this, &ReadDataClosure::readBlock));
		}else if(block_index < double_offset) {
			indexLevel0 = block_index - single_offset;

			refLevel0.reset();
			refLevel0 = ext2fs.blockCache.lock(inode->fileData.blocks.singleIndirect);
			refLevel0->waitUntilReady(CALLBACK_MEMBER(this, &ReadDataClosure::readLevel0));
		}else{
			assert(block_index < triple_offset);
			indexLevel1 = (block_index - double_offset) / per_single;
			indexLevel0 = (block_index - double_offset) % per_single;

			refLevel1.reset();
			refLevel1 = ext2fs.blockCache.lock(inode->fileData.blocks.doubleIndirect);
			refLevel1->waitUntilReady(CALLBACK_MEMBER(this, &ReadDataClosure::readLevel1));
		}
	}else{
		assert(blocksRead == numBlocks);

		callback();
		delete this;
	}
}

void FileSystem::ReadDataClosure::readLevel1() {
	auto array_level1 = (uint32_t *)refLevel1->buffer;
	uint32_t indirect = array_level1[indexLevel1];
	assert(indirect != 0);

	refLevel0.reset();
	refLevel0 = ext2fs.blockCache.lock(indirect);
	refLevel0->waitUntilReady(CALLBACK_MEMBER(this, &ReadDataClosure::readLevel0));
}

void FileSystem::ReadDataClosure::readLevel0() {
	auto array_level0 = (uint32_t *)refLevel0->buffer;
	uint32_t block = array_level0[indexLevel0];
	assert(block != 0);

	size_t per_single = ext2fs.blockSize / 4;
	chunkSize = 1;
	while(indexLevel0 + chunkSize < per_single && blocksRead + chunkSize < numBlocks) {
		// TODO: artifical limit because the virtio driver cannot handle large blocks
		if(array_level0[indexLevel0 + chunkSize] != block + chunkSize)
			break;
		chunkSize++;
	}

	ext2fs.device->readSectors(block * ext2fs.sectorsPerBlock,
			(uint8_t *)buffer + blocksRead * ext2fs.blockSize, chunkSize * ext2fs.sectorsPerBlock,
			CALLBACK_MEMBER(this, &ReadDataClosure::readBlock));
}

void FileSystem::ReadDataClosure::readBlock() {
	blocksRead += chunkSize;
	(*this)();
}

// --------------------------------------------------------
// FileSystem::BlockCacheEntry
// --------------------------------------------------------

FileSystem::BlockCacheEntry::BlockCacheEntry(void *buffer)
: buffer(buffer), state(kStateInitial) { }

void FileSystem::BlockCacheEntry::waitUntilReady(frigg::CallbackPtr<void()> callback) {
	if(state == kStateReady) {
		callback();
	}else{
		assert(state == kStateLoading);
		readyQueue.push_back(callback);
	}
}

void FileSystem::BlockCacheEntry::loadComplete() {
	assert(state == kStateLoading);
	state = kStateReady;

	for(auto it = readyQueue.begin(); it != readyQueue.end(); ++it)
		(*it)();
	readyQueue.clear();
}

// --------------------------------------------------------
// FileSystem::BlockCache
// --------------------------------------------------------

FileSystem::BlockCache::BlockCache(FileSystem &fs)
: fs(fs) { }

auto FileSystem::BlockCache::allocate() -> Element * {
	void *buffer = malloc(fs.blockSize);
	return new Element(BlockCacheEntry(buffer));
}

void FileSystem::BlockCache::initEntry(uint64_t block, BlockCacheEntry *entry) {
	assert(entry->state == BlockCacheEntry::kStateInitial);
	
	entry->state = BlockCacheEntry::kStateLoading;
	fs.device->readSectors(block * fs.sectorsPerBlock, entry->buffer, fs.sectorsPerBlock,
			CALLBACK_MEMBER(entry, &BlockCacheEntry::loadComplete));
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

} } // namespace blockfs::ext2fs

