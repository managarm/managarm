
#include <stdlib.h>
#include <stdio.h>
#include <algorithm>

#include "ext2fs.hpp"

namespace libfs {
namespace ext2fs {

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

Inode::Inode(FileSystem &fs, uint32_t number)
: fs(fs), number(number), isReady(false) { }

struct FindEntryClosure {
	FindEntryClosure(std::shared_ptr<Inode> inode, std::string name,
			frigg::CallbackPtr<void(std::experimental::optional<DirEntry>)> callback);

	void operator() ();

	void readBlocks();

	std::shared_ptr<Inode> inode;
	std::string name;
	frigg::CallbackPtr<void(std::experimental::optional<DirEntry>)> callback;

	uint8_t *blockBuffer;
};

FindEntryClosure::FindEntryClosure(std::shared_ptr<Inode> inode, std::string name,
		frigg::CallbackPtr<void(std::experimental::optional<DirEntry>)> callback)
: inode(std::move(inode)), name(name), callback(callback) { }

void FindEntryClosure::operator() () {
	blockBuffer = (uint8_t *)malloc(inode->fs.blockSize);
	inode->fs.readData(inode, 0, 1, blockBuffer,
			CALLBACK_MEMBER(this, &FindEntryClosure::readBlocks));
}

void FindEntryClosure::readBlocks() {
	uintptr_t offset = 0;
	while(offset < inode->fileSize) {
		auto disk_entry = (DiskDirEntry *)&blockBuffer[offset];
		if(strncmp(disk_entry->name, name.c_str(), disk_entry->nameLength) == 0) {
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
			return;
		}

		offset += disk_entry->recordLength;
	}
	assert(offset == inode->fileSize);

	callback(std::experimental::nullopt);
}

void Inode::findEntry(std::string name,
		frigg::CallbackPtr<void(std::experimental::optional<DirEntry>)> callback) {
	assert(!name.empty() && name != "." && name != "..");

	auto closure = new FindEntryClosure(shared_from_this(), std::move(name), callback);
	(*closure)();
}

// --------------------------------------------------------
// FileSystem
// --------------------------------------------------------

FileSystem::FileSystem(BlockDevice *device)
: device(device) {
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
	printf("Inode %u: file size: %u\n", inode->number, disk_inode->size);

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
			if(!refLevel0->ready) {
				assert(!refLevel0->loading);
				refLevel0->loading = true;
				ext2fs.device->readSectors(inode->fileData.blocks.singleIndirect
						* ext2fs.sectorsPerBlock, refLevel0->buffer, ext2fs.sectorsPerBlock,
						CALLBACK_MEMBER(this, &ReadDataClosure::readLevel0));
			}else{
				readLevel0();
			}
		}else{
			assert(block_index < triple_offset);
			indexLevel1 = (block_index - double_offset) / per_single;
			indexLevel0 = (block_index - double_offset) % per_single;

			refLevel1.reset();
			refLevel1 = ext2fs.blockCache.lock(inode->fileData.blocks.doubleIndirect);
			if(!refLevel1->ready) {
				assert(!refLevel1->loading);
				refLevel1->loading = true;
				ext2fs.device->readSectors(inode->fileData.blocks.doubleIndirect
						* ext2fs.sectorsPerBlock, refLevel1->buffer, ext2fs.sectorsPerBlock,
						CALLBACK_MEMBER(this, &ReadDataClosure::readLevel1));
			}else{
				readLevel1();
			}
		}
	}else{
		assert(blocksRead == numBlocks);

		callback();
		delete this;
	}
}

void FileSystem::ReadDataClosure::readLevel1() {
	refLevel1->ready = true;

	auto array_level1 = (uint32_t *)refLevel1->buffer;
	uint32_t indirect = array_level1[indexLevel1];
	assert(indirect != 0);

	refLevel0.reset();
	refLevel0 = ext2fs.blockCache.lock(indirect);
	if(!refLevel0->ready) {
		assert(!refLevel0->loading);
		refLevel0->loading = true;
		ext2fs.device->readSectors(indirect * ext2fs.sectorsPerBlock,
				refLevel0->buffer, ext2fs.sectorsPerBlock,
				CALLBACK_MEMBER(this, &ReadDataClosure::readLevel0));
	}else{
		readLevel0();
	}
}

void FileSystem::ReadDataClosure::readLevel0() {
	refLevel0->ready = true;

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
: loading(false), ready(false), buffer(buffer) { }

// --------------------------------------------------------
// FileSystem::BlockCache
// --------------------------------------------------------

auto FileSystem::BlockCache::allocate() -> Element * {
	// FIXME: magic number
	void *buffer = malloc(1024);
	return new Element(BlockCacheEntry(buffer));
}

void FileSystem::BlockCache::initEntry(BlockCacheEntry *entry) {
	assert(!entry->loading && !entry->ready);
}

void FileSystem::BlockCache::finishEntry(BlockCacheEntry *entry) {
	assert(entry->ready);
	entry->loading = false;
	entry->ready = false;
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
	remote.reset();

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

void Connection::recvRequest(HelError error, int64_t msg_request, int64_t msg_seq,
		size_t length) {
	HEL_CHECK(error);

	managarm::fs::CntRequest request;
	request.ParseFromArray(buffer, length);

	if(request.req_type() == managarm::fs::CntReqType::FSTAT) {
		auto closure = new StatClosure(*this, msg_request, std::move(request));
		(*closure)();
	}else if(request.req_type() == managarm::fs::CntReqType::OPEN) {
		auto closure = new OpenClosure(*this, msg_request, std::move(request));
		(*closure)();
	}else if(request.req_type() == managarm::fs::CntReqType::READ) {
		auto closure = new ReadClosure(*this, msg_request, std::move(request));
		(*closure)();
	}else if(request.req_type() == managarm::fs::CntReqType::SEEK_ABS
			|| request.req_type() == managarm::fs::CntReqType::SEEK_REL
			|| request.req_type() == managarm::fs::CntReqType::SEEK_EOF) {
		auto closure = new SeekClosure(*this, msg_request, std::move(request));
		(*closure)();
	}else{
		fprintf(stderr, "Illegal request type\n");
		abort();
	}

	(*this)();
}

// --------------------------------------------------------
// StatClosure
// --------------------------------------------------------

StatClosure::StatClosure(Connection &connection, int64_t response_id,
		managarm::fs::CntRequest request)
: connection(connection), responseId(response_id), request(std::move(request)) { }

void StatClosure::operator() () {
	openFile = connection.getOpenFile(request.fd());
	if(openFile->inode->isReady) {
		inodeReady();
	}else{
		openFile->inode->readyQueue.push_back(CALLBACK_MEMBER(this, &StatClosure::inodeReady));
	}
}

void StatClosure::inodeReady() {
	managarm::fs::SvrResponse response;
	response.set_error(managarm::fs::Errors::SUCCESS);
	response.set_file_size(openFile->inode->fileSize);

	std::string serialized;
	response.SerializeToString(&serialized);
	connection.getPipe().sendStringResp(serialized.data(), serialized.size(), responseId, 0);
}

// --------------------------------------------------------
// OpenClosure
// --------------------------------------------------------

OpenClosure::OpenClosure(Connection &connection, int64_t response_id,
		managarm::fs::CntRequest request)
: connection(connection), responseId(response_id), request(std::move(request)) { }

void OpenClosure::operator() () {
	tailPath = request.path();
	directory = connection.getFs().accessRoot();
	processSegment();
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
		managarm::fs::SvrResponse response;
		response.set_error(managarm::fs::Errors::FILE_NOT_FOUND);

		std::string serialized;
		response.SerializeToString(&serialized);
		connection.getPipe().sendStringResp(serialized.data(), serialized.size(),
				responseId, 0);
	}
	
	auto inode = connection.getFs().accessInode(entry->inode);
	if(tailPath.empty()) {
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

		std::string serialized;
		response.SerializeToString(&serialized);
		connection.getPipe().sendStringResp(serialized.data(), serialized.size(),
				responseId, 0);
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
		managarm::fs::SvrResponse response;
		response.set_error(managarm::fs::Errors::END_OF_FILE);

		std::string serialized;
		response.SerializeToString(&serialized);
		connection.getPipe().sendStringResp(serialized.data(), serialized.size(), responseId, 0);
		
		delete this;
		return;
	}
	
	if(openFile->inode->fileType == kTypeSymlink && openFile->inode->fileSize <= 60) {
		size_t read_size = std::min({ size_t(request.size()),
				size_t(openFile->inode->fileSize - openFile->offset) });

		managarm::fs::SvrResponse response;
		response.set_error(managarm::fs::Errors::SUCCESS);

		std::string serialized;
		response.SerializeToString(&serialized);
		connection.getPipe().sendStringResp(serialized.data(), serialized.size(), responseId, 0);
		connection.getPipe().sendStringResp(openFile->inode->fileData.embedded + openFile->offset,
				read_size, responseId, 1);

		openFile->offset += read_size;

		delete this;
	}else{
		size_t read_size = std::min(size_t(request.size()),
				openFile->inode->fileSize - openFile->offset);
		assert(read_size > 0);
		
		size_t block_size = openFile->inode->fs.blockSize;
		auto first_block = openFile->offset / block_size;
		auto last_block = (openFile->offset + read_size) / block_size;
		numBlocks = last_block - first_block + 1;

		blockBuffer = (char *)malloc(numBlocks * openFile->inode->fs.blockSize);
		assert(blockBuffer);
		openFile->inode->fs.readData(openFile->inode, first_block, numBlocks, blockBuffer,
				CALLBACK_MEMBER(this, &ReadClosure::readBlocks));
	}
}

void ReadClosure::readBlocks() {
	size_t read_size = std::min(size_t(request.size()),
			openFile->inode->fileSize - openFile->offset);

	size_t block_size = openFile->inode->fs.blockSize;
	size_t read_offset = openFile->offset % block_size;

	managarm::fs::SvrResponse response;
	response.set_error(managarm::fs::Errors::SUCCESS);

	std::string serialized;
	response.SerializeToString(&serialized);
	connection.getPipe().sendStringResp(serialized.data(), serialized.size(), responseId, 0);
	connection.getPipe().sendStringResp(blockBuffer + read_offset, read_size, responseId, 1);

	openFile->offset += read_size;

	free(blockBuffer);
	delete this;
}

// --------------------------------------------------------
// SeekClosure
// --------------------------------------------------------

SeekClosure::SeekClosure(Connection &connection, int64_t response_id,
		managarm::fs::CntRequest request)
: connection(connection), responseId(response_id), request(std::move(request)) { }

void SeekClosure::operator() () {
	openFile = connection.getOpenFile(request.fd());
	assert(openFile->inode->isReady);

	if(request.req_type() == managarm::fs::CntReqType::SEEK_ABS) {
		openFile->offset = request.rel_offset();
	}else if(request.req_type() == managarm::fs::CntReqType::SEEK_REL) {
		openFile->offset += request.rel_offset();
	}else if(request.req_type() == managarm::fs::CntReqType::SEEK_EOF) {
		openFile->offset = openFile->inode->fileSize;
	}else{
		printf("Illegal SEEK request");
		abort();
	}
	
	managarm::fs::SvrResponse response;
	response.set_error(managarm::fs::Errors::SUCCESS);
	response.set_offset(openFile->offset);

	std::string serialized;
	response.SerializeToString(&serialized);
	connection.getPipe().sendStringResp(serialized.data(), serialized.size(), responseId, 0);
}


} } // namespace libfs::ext2fs

