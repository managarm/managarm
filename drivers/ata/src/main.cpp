
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>

#include <memory>
#include <string>
#include <vector>
#include <queue>

#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/array.hpp>
#include <frigg/callback.hpp>
#include <frigg/arch_x86/machine.hpp>
#include <helx.hpp>

#include <bragi/mbus.hpp>

struct Allocator {
	void *allocate(size_t length) {
		return malloc(length);
	}

	void free(void *pointer) {
		free(pointer);
	}
};

Allocator allocator;

helx::EventHub eventHub = helx::EventHub::create();
bragi_mbus::Connection mbusConnection(eventHub);

// --------------------------------------------------------
// Driver class
// --------------------------------------------------------

class Driver {
public:
	Driver();

	void readSectors(int64_t sector, void *buffer,
			size_t num_sectors, frigg::CallbackPtr<void()> callback);

private:
	enum Ports {
		kPortReadData = 0,
		kPortWriteSectorCount = 2,
		kPortWriteLba1 = 3,
		kPortWriteLba2 = 4,
		kPortWriteLba3 = 5,
		kPortWriteDevice = 6,
		kPortWriteCommand = 7,
		kPortReadStatus = 7,
	};

	enum Commands {
		kCommandReadSectorsExt = 0x24
	};

	enum Flags {
		kStatusDrq = 0x08,
		kStatusBsy = 0x80,

		kDeviceSlave = 0x10,
		kDeviceLba = 0x40
	};

	struct Request {
		int64_t sector;
		size_t numSectors;
		size_t sectorsRead;
		void *buffer;
		frigg::CallbackPtr<void()> callback;
	};
	
	void performRequest();
	void onReadIrq(HelError error);

	std::queue<Request> p_requestQueue;

	HelHandle p_irqHandle;
	HelHandle p_ioHandle;
	uint16_t p_basePort;
	bool p_inRequest;
};

Driver::Driver()
: p_basePort(0x1F0), p_inRequest(false) {
	HEL_CHECK(helAccessIrq(14, &p_irqHandle));

	uintptr_t ports[] = { 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6 };
	HEL_CHECK(helAccessIo(ports, 9, &p_ioHandle));
	HEL_CHECK(helEnableIo(p_ioHandle));
}

void Driver::readSectors(int64_t sector, void *buffer,
			size_t num_sectors, frigg::CallbackPtr<void()> callback) {
	Request request;
	request.sector = sector;
	request.numSectors = num_sectors;
	request.sectorsRead = 0;
	request.buffer = buffer;
	request.callback = callback;
	p_requestQueue.push(request);

	if(!p_inRequest)
		performRequest();
}

void Driver::performRequest() {
	p_inRequest = true;

	Request &request = p_requestQueue.front();

	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteDevice, kDeviceLba);
	
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteSectorCount, (request.numSectors >> 8) & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba1, (request.sector >> 24) & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba2, (request.sector >> 32) & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba3, (request.sector >> 40) & 0xFF);	
	
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteSectorCount, request.numSectors & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba1, request.sector & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba2, (request.sector >> 8) & 0xFF);
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteLba3, (request.sector >> 16) & 0xFF);
	
	frigg::arch_x86::ioOutByte(p_basePort + kPortWriteCommand, kCommandReadSectorsExt);

	auto callback = CALLBACK_MEMBER(this, &Driver::onReadIrq);
	int64_t async_id;
	HEL_CHECK(helSubmitWaitForIrq(p_irqHandle, eventHub.getHandle(),
			(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
			&async_id));
}

void Driver::onReadIrq(HelError error) {
	Request &request = p_requestQueue.front();
	
	// acknowledge the interrupt
	/*uint8_t status =*/ frigg::arch_x86::ioInByte(p_basePort + kPortReadStatus);
//	assert((status & kStatusBsy) == 0);
//	assert((status & kStatusDrq) != 0);
	
	size_t offset = request.sectorsRead * 512;
	auto ptr = (uint8_t *)request.buffer + offset;
	for(int i = 0; i < 256; i++) {
		uint16_t word = frigg::arch_x86::ioInShort(p_basePort + kPortReadData);
		ptr[2 * i] = word & 0xFF;
		ptr[2 * i + 1] = (word >> 8) & 0xFF;
	}
	
	request.sectorsRead++;
	if(request.sectorsRead < request.numSectors) {
		auto callback = CALLBACK_MEMBER(this, &Driver::onReadIrq);
		int64_t async_id;
		HEL_CHECK(helSubmitWaitForIrq(p_irqHandle, eventHub.getHandle(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
				&async_id));
	}else{
		assert(request.sectorsRead == request.numSectors);
		request.callback();
		p_requestQueue.pop();

		p_inRequest = false;
		if(!p_requestQueue.empty())
			performRequest();
	}
}

// --------------------------------------------------------
// GptTable
// --------------------------------------------------------

struct GptTable {
public:
	struct Partition {

		void readSectors(int64_t sector, void *buffer,
				size_t num_sectors, frigg::CallbackPtr<void()> callback);

		Partition(GptTable &gpt_table, uint64_t start_lba, uint64_t num_sectors);
		
		GptTable &gptTable;
		uint64_t startLba;
		uint64_t numSectors;
	};

	GptTable(Driver &device);

	void parse(frigg::CallbackPtr<void()> callback);

	Partition &getPartition(int index);

private:
	struct GptHeader {
		uint64_t signature;
		uint32_t revision;
		uint32_t headerSize;
		uint32_t headerCheckSum;
		uint32_t reservedZero;
		uint64_t currentLba;
		uint64_t backupLba;
		uint64_t firstLba;
		uint64_t lastLba;
		uint8_t diskGuid[16];
		uint64_t startingLba;
		uint32_t numEntries;
		uint32_t entrySize;
		uint32_t tableCheckSum;
		uint8_t padding[420];
	};
	static_assert(sizeof(GptHeader) == 512, "Bad GPT header struct size");

	struct GptEntry {
		uint8_t typeGuid[16];
		uint8_t uniqueGuid[16];
		uint64_t firstLba;
		uint64_t lastLba;
		uint64_t attrFlags;
		uint8_t partitionName[72];	
	};
	static_assert(sizeof(GptEntry) == 128, "Bad GPT entry struct size");

	struct ParseClosure {
		ParseClosure(GptTable &gpt_table, frigg::CallbackPtr<void()> callback);

		void operator() ();
	
	private:
		void readHeader();
		void readTable();

		GptTable &gptTable;
		void *headerBuffer;
		void *tableBuffer;
		frigg::CallbackPtr<void()> callback;
	};

	Driver &device;
	std::vector<Partition> partitions;
};

GptTable::GptTable(Driver &device)
: device(device) { }

void GptTable::parse(frigg::CallbackPtr<void()> callback) {
	auto closure = new ParseClosure(*this, callback);
	(*closure)();
}

GptTable::Partition &GptTable::getPartition(int index) {
	return partitions[index];
}

// --------------------------------------------------------
// GptTable::Partition
// --------------------------------------------------------

GptTable::Partition::Partition(GptTable &gpt_table, uint64_t start_lba, uint64_t num_sectors)
: gptTable(gpt_table), startLba(start_lba), numSectors(num_sectors) { }

void GptTable::Partition::readSectors(int64_t sector, void *buffer,
		size_t num_read_sectors, frigg::CallbackPtr<void()> callback) {
	assert(sector + num_read_sectors <= numSectors);
	gptTable.device.readSectors(startLba + sector, buffer, num_read_sectors, callback);
}

// --------------------------------------------------------
// GptTable::ParseClosure
// --------------------------------------------------------

GptTable::ParseClosure::ParseClosure(GptTable &gpt_table, frigg::CallbackPtr<void()> callback)
: gptTable(gpt_table), callback(callback) { }

void GptTable::ParseClosure::operator() () {
	headerBuffer = malloc(512);
	assert(headerBuffer);
	gptTable.device.readSectors(1, headerBuffer, 1,
			CALLBACK_MEMBER(this, &ParseClosure::readHeader));
}

void GptTable::ParseClosure::readHeader() {
	GptHeader *header = (GptHeader *)headerBuffer;
	assert(header->signature == 0x5452415020494645); // TODO: handle this error

	printf("%u entries, %u bytes per entry\n", header->numEntries, header->entrySize);

	size_t table_size = header->entrySize * header->numEntries;
	size_t table_sectors = table_size / 512;
	if(!(table_size % 512))
		table_sectors++;

	tableBuffer = malloc(table_sectors * 512);
	assert(tableBuffer);
	gptTable.device.readSectors(2, tableBuffer, table_sectors,
			CALLBACK_MEMBER(this, &ParseClosure::readTable));
}

void GptTable::ParseClosure::readTable() {
	GptHeader *header = (GptHeader *)headerBuffer;

	for(uint32_t i = 0; i < header->numEntries; i++) {
		GptEntry *entry = (GptEntry *)((uintptr_t)tableBuffer + i * header->entrySize);
		
		bool all_zeros = true;
		for(int j = 0; j < 16; j++)
			if(entry->typeGuid[j] != 0)
				all_zeros = false;
		if(all_zeros)
			continue;

		printf("start: %lu, end: %lu \n", entry->firstLba, entry->lastLba);
		gptTable.partitions.push_back(GptTable::Partition(gptTable, entry->firstLba,
				entry->lastLba - entry->firstLba + 1));
	}

	callback();
	
	free(headerBuffer);
	free(tableBuffer);
	delete this;
}

// --------------------------------------------------------
// Ext2fs
// --------------------------------------------------------

struct Ext2fs {
	struct Inode {
		Inode(uint32_t number);

		// ext2fs on-disk inode number
		const uint32_t number;
		
		// true if this inode has already been loaded from disk
		bool isReady;
		
		// called when the inode becomes ready
		std::vector<frigg::CallbackPtr<void()>> readyQueue;
		
		// NOTE: The following fields are only meaningful if the isReady is true

		// file size in bytes
		uint64_t fileSize;

		// data block pointers
		uint32_t directBlocks[12];
	};

	Ext2fs(GptTable::Partition &partition);

	void init(frigg::CallbackPtr<void()> callback);	

	std::shared_ptr<Inode> accessRoot();

	void readData(std::shared_ptr<Inode> inode, uint64_t block_offset,
			size_t num_blocks, void *buffer, frigg::CallbackPtr<void()> callback);

private:
	struct DiskSuperblock {
		uint32_t inodesCount;
		uint32_t blocksCount;
		uint32_t rBlocksCount;
		uint32_t freeBlocksCount;
		uint32_t freeInodesCount;
		uint32_t firstDataBlock;
		uint32_t logBlockSize;
		uint32_t logFragSize;
		uint32_t blocksPerGroup;
		uint32_t fragsPerGroup;
		uint32_t inodesPerGroup;
		uint32_t mtime;
		uint32_t wtime;
		uint16_t mntCount;
		uint16_t maxMntCount;
		uint16_t magic;
		uint16_t state;
		uint16_t errors;
		uint16_t minorRevLevel;
		uint32_t lastcheck;
		uint32_t checkinterval;
		uint32_t creatorOs;
		uint32_t revLevel;
		uint16_t defResuid;
		uint16_t defResgid;
		//-- EXT2_DYNAMIC_REV Specific --
		uint32_t firstIno;
		uint16_t inodeSize;
		uint16_t blockGroupNr;
		uint32_t featureCompat;
		uint32_t featureIncompat;
		uint32_t featureRoCompat;
		uint8_t uuid[16];
		uint8_t volumeName[16];
		uint8_t lastMounted[64];
		uint32_t algoBitmap;
		//-- Performance Hints --
		uint8_t preallocBlocks;
		uint8_t preallocDirBlocks;
		uint16_t alignment;
		//-- Journaling Support --
		uint8_t journalUuid[16];
		uint32_t journalInum;
		uint32_t journalDev;
		uint32_t lastOrphan;
		//-- Directory Indexing Support --
		uint32_t hashSeed[4];
		uint8_t defHashVersion;
		uint8_t padding[3];
		//-- Other options --
		uint32_t defaultMountOptions;
		uint32_t firstMetaBg;
		uint8_t unused[760];
	};
	static_assert(sizeof(DiskSuperblock) == 1024, "Bad DiskSuperblock struct size");

	struct DiskGroupDesc {
		uint32_t blockBitmap;
		uint32_t inodeBitmap;
		uint32_t inodeTable;
		uint16_t freeBlocksCount;
		uint16_t freeInodesCount;
		uint16_t usedDirsCount;
		uint16_t pad;
		uint8_t reserved[12];
	};
	static_assert(sizeof(DiskGroupDesc) == 32, "Bad DiskGroupDesc struct size");

	struct DiskInode {
		uint16_t mode;
		uint16_t uid;
		uint32_t size;
		uint32_t atime;
		uint32_t ctime;
		uint32_t mtime;
		uint32_t dtime;
		uint16_t gid;
		uint16_t linksCount;
		uint32_t blocks;
		uint32_t flags;
		uint32_t osdl;
		uint32_t directBlocks[12];
		uint32_t indirectBlock;
		uint32_t indirect2Block;
		uint32_t indirect3Block;
		uint32_t generation;
		uint32_t fileAcl;
		uint32_t dirAcl;
		uint32_t faddr;
		uint8_t osd2[12];
	};
	static_assert(sizeof(DiskInode) == 128, "Bad DiskInode struct size");

	enum {
		EXT2_ROOT_INO = 2
	};

	struct InitClosure {
		InitClosure(Ext2fs &ext2fs, frigg::CallbackPtr<void()> callback);

		void operator() ();
	
	private:
		void readSuperblock();
		void readBlockGroups();

		Ext2fs &ext2fs;
		frigg::CallbackPtr<void()> callback;
		uint8_t superblockBuffer[1024];
	};

	struct ReadInodeClosure {
		ReadInodeClosure(Ext2fs &ext2fs, std::shared_ptr<Inode> inode);

		void operator() ();
	
	private:
		void readSector();

		Ext2fs &ext2fs;
		std::shared_ptr<Inode> inode;
		uint8_t sectorBuffer[512];
	};

	struct ReadDataClosure {
		ReadDataClosure(Ext2fs &ext2fs, std::shared_ptr<Inode> inode,
				uint64_t block_offset, size_t num_blocks, void *buffer,
				frigg::CallbackPtr<void()> callback);
		
		void operator() ();

	private:
		void readBlock();

		Ext2fs &ext2fs;
		std::shared_ptr<Inode> inode;
		uint64_t blockOffset;
		size_t numBlocks;
		void *buffer;
		frigg::CallbackPtr<void()> callback;

		size_t blocksRead;
	};
	
	GptTable::Partition &partition;
	uint16_t inodeSize;
	uint32_t blockSize;
	uint32_t sectorsPerBlock;
	uint32_t numBlockGroups;
	uint32_t inodesPerGroup;
	void *blockGroupDescriptorBuffer;

	std::weak_ptr<Inode> cachedRoot;
};

Ext2fs::Ext2fs(GptTable::Partition &partition)
: partition(partition) { }

void Ext2fs::init(frigg::CallbackPtr<void()> callback) {
	auto closure = new InitClosure(*this, callback);
	(*closure)();
}

auto Ext2fs::accessRoot() -> std::shared_ptr<Inode> {
	std::shared_ptr<Inode> try_root = cachedRoot.lock();
	if(try_root)
		return std::move(try_root);
	
	auto new_root = std::make_shared<Inode>(EXT2_ROOT_INO);
	cachedRoot = new_root;

	auto closure = new ReadInodeClosure(*this, new_root);
	(*closure)();

	return std::move(new_root);
}

void Ext2fs::readData(std::shared_ptr<Inode> inode, uint64_t block_offset,
		size_t num_blocks, void *buffer, frigg::CallbackPtr<void()> callback) {
	auto closure = new ReadDataClosure(*this, std::move(inode), block_offset,
			num_blocks, buffer, callback);
	(*closure)();
}

// --------------------------------------------------------
// Ext2fs::Inode
// --------------------------------------------------------

Ext2fs::Inode::Inode(uint32_t number)
: number(number), isReady(false) { }

// --------------------------------------------------------
// Ext2fs::InitClosure
// --------------------------------------------------------

Ext2fs::InitClosure::InitClosure(Ext2fs &ext2fs, frigg::CallbackPtr<void()> callback)
: ext2fs(ext2fs), callback(callback) { }

void Ext2fs::InitClosure::operator() () {
	ext2fs.partition.readSectors(2, superblockBuffer, 2,
			CALLBACK_MEMBER(this, &InitClosure::readSuperblock));
}

void Ext2fs::InitClosure::readSuperblock() {
	DiskSuperblock *sb = (DiskSuperblock *)superblockBuffer;
	assert(sb->magic == 0xEF53);

	ext2fs.inodeSize = sb->inodeSize;
	ext2fs.blockSize = 1024 << sb->logBlockSize;
	ext2fs.sectorsPerBlock = ext2fs.blockSize / 512;
	ext2fs.numBlockGroups = sb->blocksCount / sb->blocksPerGroup;
	ext2fs.inodesPerGroup = sb->inodesPerGroup;

	printf("magic: %x \n", sb->magic);
	printf("numBlockGroups: %d \n", ext2fs.numBlockGroups);
	printf("blockSize: %d \n", ext2fs.blockSize);

	size_t bgdt_size = ext2fs.numBlockGroups * sizeof(DiskGroupDesc);
	if(bgdt_size % 512)
		bgdt_size += 512 - (bgdt_size % 512);
	ext2fs.blockGroupDescriptorBuffer = malloc(bgdt_size);

	ext2fs.partition.readSectors(2 * ext2fs.sectorsPerBlock,
			ext2fs.blockGroupDescriptorBuffer, bgdt_size / 512,
			CALLBACK_MEMBER(this, &InitClosure::readBlockGroups));
}

void Ext2fs::InitClosure::readBlockGroups() {
	DiskGroupDesc *bgdt = (DiskGroupDesc *)ext2fs.blockGroupDescriptorBuffer;
	printf("bgBlockBitmap: %x \n", bgdt->blockBitmap);

	callback();
	delete this;
};

// --------------------------------------------------------
// Ext2fs::ReadInodeClosure
// --------------------------------------------------------

Ext2fs::ReadInodeClosure::ReadInodeClosure(Ext2fs &ext2fs, std::shared_ptr<Inode> inode)
: ext2fs(ext2fs), inode(std::move(inode)) { }	

void Ext2fs::ReadInodeClosure::operator() () {
	uint32_t block_group = (inode->number - 1) / ext2fs.inodesPerGroup;
	uint32_t index = (inode->number - 1) % ext2fs.inodesPerGroup;

	auto bgdt = (DiskGroupDesc *)ext2fs.blockGroupDescriptorBuffer;
	uint32_t inode_table_block = bgdt[block_group].inodeTable;
	uint32_t offset = index * ext2fs.inodeSize;

	uint32_t sector = inode_table_block * ext2fs.sectorsPerBlock + (offset / 512);
	ext2fs.partition.readSectors(sector, sectorBuffer, 1,
			CALLBACK_MEMBER(this, &ReadInodeClosure::readSector));
}

void Ext2fs::ReadInodeClosure::readSector() {
	uint32_t index = (inode->number - 1) % ext2fs.inodesPerGroup;
	uint32_t offset = index * ext2fs.inodeSize;

	DiskInode *disk_inode = (DiskInode *)(sectorBuffer + (offset % 512));
	printf("mode: %x , offset: %d , inodesize: %d \n", disk_inode->mode, offset, ext2fs.inodeSize);

	// TODO: support large files
	inode->fileSize = disk_inode->size;
	for(int i = 0; i < 12; i++)
		inode->directBlocks[i] = disk_inode->directBlocks[i];
	
	inode->isReady = true;
	for(auto it = inode->readyQueue.begin(); it != inode->readyQueue.end(); ++it)
		(*it)();
	inode->readyQueue.clear();

	delete this;
}

// --------------------------------------------------------
// Ext2fs::ReadDataClosure
// --------------------------------------------------------

Ext2fs::ReadDataClosure::ReadDataClosure(Ext2fs &ext2fs, std::shared_ptr<Inode> inode,
		uint64_t block_offset, size_t num_blocks, void *buffer,
		frigg::CallbackPtr<void()> callback)
: ext2fs(ext2fs), inode(std::move(inode)), blockOffset(block_offset),
		numBlocks(num_blocks), buffer(buffer), callback(callback),
		blocksRead(0) { }

void Ext2fs::ReadDataClosure::operator() () {
	if(blocksRead < numBlocks) {
		uint32_t direct_index = blockOffset + blocksRead;
		uint32_t block = inode->directBlocks[direct_index];

		printf("reading block %u, %lu of %lu complete!\n", block, blocksRead, numBlocks);
		ext2fs.partition.readSectors(block * ext2fs.sectorsPerBlock,
				(uint8_t *)buffer + blocksRead * ext2fs.blockSize, ext2fs.sectorsPerBlock,
				CALLBACK_MEMBER(this, &ReadDataClosure::readBlock));
	}else{
		callback();
		delete this;
	}
}

void Ext2fs::ReadDataClosure::readBlock() {
	blocksRead++;
	(*this)();
}

// --------------------------------------------------------

// --------------------------------------------------------

Driver ataDriver;
GptTable gptTable(ataDriver);
Ext2fs *fs;
std::shared_ptr<Ext2fs::Inode> rootInode;
uint8_t dataBuffer[512];

struct DirEntry {
	uint32_t inode;
	uint16_t recordLength;
	uint8_t nameLength;
	uint8_t fileType;
	char name[];
};

void done4(void *object) {
	uintptr_t offset = 0;
	while(offset < rootInode->fileSize) {
		DirEntry *entry = (DirEntry *)&dataBuffer[offset];

		printf("File: %.*s\n", entry->nameLength, entry->name);

		offset += entry->recordLength;
	}
}

void done3(void *object) {
	printf("Root file size: %lu\n", rootInode->fileSize);
	fs->readData(rootInode, 0, 1, dataBuffer, CALLBACK_STATIC(nullptr, &done4));
}

void done2(void *object) {
	rootInode = fs->accessRoot();
	rootInode->readyQueue.push_back(CALLBACK_STATIC(nullptr, &done3));
}

void done(void *object) {
	fs = new Ext2fs(gptTable.getPartition(1));
	fs->init(CALLBACK_STATIC(nullptr, &done2));
}

void initAta() {
	gptTable.parse(CALLBACK_STATIC(nullptr, &done));
}

// --------------------------------------------------------
// ObjectHandler
// --------------------------------------------------------

struct ObjectHandler : public bragi_mbus::ObjectHandler {
	// inherited from bragi_mbus::ObjectHandler
	void requireIf(bragi_mbus::ObjectId object_id,
			frigg::CallbackPtr<void(HelHandle)> callback);
};

void ObjectHandler::requireIf(bragi_mbus::ObjectId object_id,
		frigg::CallbackPtr<void(HelHandle)> callback) {
	helx::Pipe server_side, client_side;
	helx::Pipe::createFullPipe(server_side, client_side);
	callback(client_side.getHandle());
	client_side.reset();
}

ObjectHandler objectHandler;

// --------------------------------------------------------
// InitClosure
// --------------------------------------------------------

struct InitClosure : public frigg::BaseClosure<InitClosure> {
	void operator() ();

private:
	void connected();
	void registered(bragi_mbus::ObjectId object_id);
};

void InitClosure::operator() () {
	mbusConnection.connect(CALLBACK_MEMBER(this, &InitClosure::connected));
}

void InitClosure::connected() {
	mbusConnection.registerObject("block-device",
			CALLBACK_MEMBER(this, &InitClosure::registered));
}

void InitClosure::registered(bragi_mbus::ObjectId object_id) {
	// do nothing for now
}

// --------------------------------------------------------
// main() function
// --------------------------------------------------------

int main() {
	printf("Starting ATA driver\n");

	initAta();
	
	mbusConnection.setObjectHandler(&objectHandler);
	frigg::runClosure<InitClosure>(allocator);

	while(true)
		eventHub.defaultProcessEvents();
}

