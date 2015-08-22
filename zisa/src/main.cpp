
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#include <frigg/traits.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>
#include <frigg/callback.hpp>
#include <frigg/async.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>

#include <queue>
#include <vector>
#include <stdexcept>

struct LibcAllocator {
	void *allocate(size_t length) {
		return malloc(length);
	}

	void free(void *pointer) {
		free(pointer);
	}
};

LibcAllocator allocator;

namespace util = frigg::util;
namespace async = frigg::async;

uint8_t ioInByte(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t out_value asm("al");
	asm volatile ( "inb %%dx, %%al" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}
uint16_t ioInShort(uint16_t port) {
	register uint16_t in_port asm("dx") = port;
	register uint16_t out_value asm("ax");
	asm volatile ( "inw %%dx, %%ax" : "=r" (out_value) : "r" (in_port) );
	return out_value;
}

void ioOutByte(uint16_t port, uint8_t value) {
	register uint16_t in_port asm("dx") = port;
	register uint8_t in_value asm("al") = value;
	asm volatile ( "outb %%al, %%dx" : : "r" (in_port), "r" (in_value) );
}

class AtaDriver {
public:
	AtaDriver(helx::EventHub &event_hub);

	void readSectors(int64_t sector, uint8_t *buffer,
			size_t num_sectors, util::Callback<void()> callback);

private:
	void performRequest();
	void onReadIrq(HelError error);

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
		uint8_t *buffer;
		util::Callback<void()> callback;
	};

	std::queue<Request> p_requestQueue;

	helx::EventHub &p_eventHub;
	HelHandle p_irqHandle;
	HelHandle p_ioHandle;
	uint16_t p_basePort;
	bool p_inRequest;
};

AtaDriver::AtaDriver(helx::EventHub &event_hub)
		: p_eventHub(event_hub), p_basePort(0x1F0), p_inRequest(false) {
	HEL_CHECK(helAccessIrq(14, &p_irqHandle));

	uintptr_t ports[] = { 0x1F0, 0x1F1, 0x1F2, 0x1F3, 0x1F4, 0x1F5, 0x1F6, 0x1F7, 0x3F6 };
	HEL_CHECK(helAccessIo(ports, 9, &p_ioHandle));
	HEL_CHECK(helEnableIo(p_ioHandle));
}

void AtaDriver::readSectors(int64_t sector, uint8_t *buffer,
			size_t num_sectors, util::Callback<void()> callback) {
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

void AtaDriver::performRequest() {
	p_inRequest = true;

	Request &request = p_requestQueue.front();

	auto callback = CALLBACK_MEMBER(this, &AtaDriver::onReadIrq);
	int64_t async_id;
	HEL_CHECK(helSubmitWaitForIrq(p_irqHandle, p_eventHub.getHandle(),
			(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
			&async_id));

	ioOutByte(p_basePort + kPortWriteDevice, kDeviceLba);
	
	ioOutByte(p_basePort + kPortWriteSectorCount, (request.numSectors >> 8) & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba1, (request.sector >> 24) & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba2, (request.sector >> 32) & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba3, (request.sector >> 40) & 0xFF);	
	
	ioOutByte(p_basePort + kPortWriteSectorCount, request.numSectors & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba1, request.sector & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba2, (request.sector >> 8) & 0xFF);
	ioOutByte(p_basePort + kPortWriteLba3, (request.sector >> 16) & 0xFF);
	
	ioOutByte(p_basePort + kPortWriteCommand, kCommandReadSectorsExt);
}

void AtaDriver::onReadIrq(HelError error) {
	Request &request = p_requestQueue.front();
	if(request.sectorsRead + 1 < request.numSectors) {
		auto callback = CALLBACK_MEMBER(this, &AtaDriver::onReadIrq);
		int64_t async_id;
		HEL_CHECK(helSubmitWaitForIrq(p_irqHandle, p_eventHub.getHandle(),
				(uintptr_t)callback.getFunction(), (uintptr_t)callback.getObject(),
				&async_id));
	}

	uint8_t status = ioInByte(p_basePort + kPortReadStatus);
//	assert((status & kStatusBsy) == 0);
//	assert((status & kStatusDrq) != 0);
	
	size_t offset = request.sectorsRead * 512;
	for(int i = 0; i < 256; i++) {
		uint16_t word = ioInShort(p_basePort + kPortReadData);
		request.buffer[offset + 2 * i] = word & 0xFF;
		request.buffer[offset + 2 * i + 1] = (word >> 8) & 0xFF;
	}
	
	request.sectorsRead++;
	if(request.sectorsRead == request.numSectors) {
		// acknowledge the interrupt
		ioInByte(p_basePort + kPortReadStatus);

		request.callback();

		p_requestQueue.pop();

		p_inRequest = false;
		if(!p_requestQueue.empty())
			performRequest();
	}
}

class Keyboard {
public:
	Keyboard(helx::EventHub &event_hub);
	
	void run();

private:
	void onScancode(int64_t submit_id);

	helx::EventHub &p_eventHub;
	HelHandle p_irqHandle;
	HelHandle p_ioHandle;
};

Keyboard::Keyboard(helx::EventHub &event_hub)
		: p_eventHub(event_hub) {
	HEL_CHECK(helAccessIrq(1, &p_irqHandle));
	
	uintptr_t ports[] = { 0x60, 0x64 };
	HEL_CHECK(helAccessIo(ports, 2, &p_ioHandle));
	HEL_CHECK(helEnableIo(p_ioHandle));
}

void Keyboard::run() {
	auto callback = CALLBACK_MEMBER(this, &Keyboard::onScancode);
	int64_t async_id;
	HEL_CHECK(helSubmitWaitForIrq(p_irqHandle, p_eventHub.getHandle(),
		(uintptr_t)callback.getFunction(),
		(uintptr_t)callback.getObject(),
		&async_id));
}

void Keyboard::onScancode(int64_t submit_id) {
	while(true) {
		uint8_t status = ioInByte(0x64);
		if((status & 0x01) == 0)
			break;

		uint8_t code = ioInByte(0x60);
		printf("0x%X\n", code);
	}

	run();
}

helx::EventHub eventHub;

// --------------------------------------------------------
// ATA testing code
// --------------------------------------------------------

AtaDriver ataDriver(eventHub);

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

struct GptParser {
public:
	struct Partition {
		void readSectors(int64_t sector, uint8_t *buffer,
				size_t num_sectors, util::Callback<void()> callback);

		Partition(uint64_t start_lba, uint64_t num_sectors);

		uint64_t startLba;
		uint64_t numSectors;
	};

	void parse(util::Callback<void()> callback);

	Partition &getPartition(int index);

	std::vector<Partition> partitions;
};

struct ParseContext {
	ParseContext(GptParser &parser, util::Callback<void()> callback)
	: parser(parser), callback(callback) { }

	GptParser &parser;
	uint8_t headerBuffer[512];
	uint8_t *tableBuffer;
	util::Callback<void()> callback;
};

auto doParse = async::seq(
	async::lambda([] (ParseContext &context, util::Callback<void()> callback) {
		ataDriver.readSectors(1, context.headerBuffer, 1, callback);
	}),
	async::lambda([] (ParseContext &context, util::Callback<void()> callback) {
		GptHeader *header = (GptHeader *)context.headerBuffer;
		if(header->signature != 0x5452415020494645)
			printf("Illegal GPT signature\n");

		size_t table_size = header->entrySize * header->numEntries;
		size_t table_sectors = table_size / 512;
		if(table_size % 512 != 0)
			table_sectors++;

		context.tableBuffer = new uint8_t[table_sectors * 512];

		printf("%u entries, %u bytes per entry\n", header->numEntries, header->entrySize);
		ataDriver.readSectors(2, context.tableBuffer, table_sectors, callback);
	}),
	async::lambda([] (ParseContext &context, util::Callback<void()> callback) {
		GptHeader *header = (GptHeader *)context.headerBuffer;

		for (int i = 0; i < header->numEntries; i++) {
			GptEntry *entry = (GptEntry *)(context.tableBuffer + i * header->entrySize);
			
			bool all_zeros = true;
			for(int j = 0; j < 16; j++)
				if(entry->typeGuid[j] != 0)
					all_zeros = false;
			if(all_zeros)
				continue;

			printf("start: %d, end: %d \n", entry->firstLba, entry->lastLba);
			GptParser &parser = context.parser;
			parser.partitions.push_back(GptParser::Partition(entry->firstLba,
					entry->lastLba - entry->firstLba + 1));
		}

		callback();
	})
);

void GptParser::parse(util::Callback<void()> callback) {
	auto on_complete = [] (ParseContext &context) {
		context.callback();
	};

	async::run(allocator, doParse, ParseContext(*this, callback), on_complete);
}

GptParser::Partition &GptParser::getPartition(int index) {
	return partitions[index];
}

GptParser::Partition::Partition(uint64_t start_lba, uint64_t num_sectors)
: startLba(start_lba), numSectors(num_sectors) { }

void GptParser::Partition::readSectors(int64_t sector, uint8_t *buffer,
		size_t sectors_to_read, util::Callback<void()> callback) {
	if(sector + sectors_to_read > numSectors)
		throw std::runtime_error("Sector not in partition");
	ataDriver.readSectors(startLba + sector, buffer, sectors_to_read, callback);
}

GptParser parser;


// --------------------------------------------------------
// Superblock testing code
// --------------------------------------------------------

struct Superblock {
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
static_assert(sizeof(Superblock) == 1024, "Bad Superblock struct size");

struct BlockGroupDescriptor {
	uint32_t blockBitmap;
	uint32_t inodeBitmap;
	uint32_t inodeTable;
	uint16_t freeBlocksCount;
	uint16_t freeInodesCount;
	uint16_t usedDirsCount;
	uint16_t pad;
	uint8_t reserved[12];
};
static_assert(sizeof(BlockGroupDescriptor) == 32, "Bad BlockGroupDescriptor struct size");

struct BlockBitmap {

};

struct InodeBitmap {

};

struct Inode {
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
static_assert(sizeof(Inode) == 128, "Bad Inode struct size");

class Ext2Driver {
friend class GptParser;
public:
	void init(util::Callback<void()> callback);	

	void readInode(uint32_t inode_number, util::Callback<void(Inode)> callback);

	void readData(Inode inode, void *buffer, uintptr_t first_block, size_t num_blocks,
			util::Callback<void()> callback);
public:
	uint16_t inodeSize;
	uint32_t blockSize;
	uint32_t sectorsPerBlock;
	uint32_t numBlockGroups;
	uint32_t inodesPerGroup;
	uint8_t *blockGroupDescriptorBuffer;
};

struct InitContext {
	InitContext(Ext2Driver &driver, util::Callback<void()> callback)
	: driver(driver), callback(callback) { }	

	Ext2Driver &driver;
	uint8_t superblockBuffer[1024];

	util::Callback<void()> callback;
};

auto doInit = async::seq(
	async::lambda([](InitContext &context, util::Callback<void()> callback) {
		parser.getPartition(1).readSectors(2, context.superblockBuffer, 2, callback);
	}),
	async::lambda([](InitContext &context, util::Callback<void()> callback) {

		Superblock *sb = (Superblock *)context.superblockBuffer;
		context.driver.inodeSize = sb->inodeSize;
		context.driver.blockSize = 1024 << sb->logBlockSize;
		context.driver.sectorsPerBlock = context.driver.blockSize / 512;
		context.driver.numBlockGroups = sb->blocksCount / sb->blocksPerGroup;
		context.driver.inodesPerGroup = sb->inodesPerGroup;

		printf("magic: %x \n", sb->magic);
		printf("numBlockGroups: %d \n", context.driver.numBlockGroups);
		printf("blockSize: %d \n", context.driver.blockSize);

		size_t bgdt_size = context.driver.numBlockGroups * sizeof(BlockGroupDescriptor);
		if((bgdt_size % 512) != 0)
			bgdt_size += 512 - (bgdt_size % 512);
		context.driver.blockGroupDescriptorBuffer = new uint8_t[bgdt_size];

		parser.getPartition(1).readSectors(2 * context.driver.sectorsPerBlock,
				context.driver.blockGroupDescriptorBuffer, bgdt_size / 512, callback);
	}),
	async::lambda([] (InitContext &context, util::Callback<void()> callback){
		BlockGroupDescriptor *bgdt = (BlockGroupDescriptor *)context.driver.blockGroupDescriptorBuffer;
		printf("bgBlockBitmap: %x \n", bgdt->blockBitmap);

		callback();
	})
);

void Ext2Driver::init(util::Callback<void()> callback) {
	auto on_complete = [] (InitContext &context) {
		context.callback();
	};

	async::run(allocator, doInit, InitContext(*this, callback), on_complete);
}


struct ReadInodeContext {
	ReadInodeContext(Ext2Driver &driver, uint32_t inode_number, util::Callback<void(Inode)> callback)
	: driver(driver), inodeNumber(inode_number), callback(callback) { }	

	Ext2Driver &driver;
	uint32_t inodeNumber;
	uint8_t inodeBuffer[512];

	util::Callback<void(Inode)> callback;
};

auto doReadInode = async::seq(
	async::lambda([] (ReadInodeContext &context, util::Callback<void()> callback){
		uint32_t block_group = (context.inodeNumber - 1) / context.driver.inodesPerGroup;
		uint32_t index = (context.inodeNumber - 1) % context.driver.inodesPerGroup;

		BlockGroupDescriptor *bgdt = (BlockGroupDescriptor *)context.driver.blockGroupDescriptorBuffer;
		uint32_t inode_table_block = bgdt[block_group].inodeTable;
		uint32_t offset = index * context.driver.inodeSize;

		uint32_t sector = inode_table_block * context.driver.sectorsPerBlock + (offset / 512);
		parser.getPartition(1).readSectors(sector, context.inodeBuffer, 1, callback);
	}),
	async::lambda([] (ReadInodeContext &context, util::Callback<void(Inode)> callback){
		uint32_t index = (context.inodeNumber - 1) % context.driver.inodesPerGroup;
		uint32_t offset = index * context.driver.inodeSize;

		Inode *inode = (Inode *)(context.inodeBuffer + (offset % 512));
		printf("mode: %x , offset: %d , inodesize: %d \n", inode->mode, offset, context.driver.inodeSize);
		callback(*inode);
	})
);

void Ext2Driver::readInode(uint32_t inode_number, util::Callback<void(Inode)> callback) {
	auto on_complete = [] (ReadInodeContext &context, Inode inode) {
		context.callback(inode);
	};

	async::run(allocator, doReadInode, ReadInodeContext(*this, inode_number,callback), on_complete);
}


struct ReadDataContext {
	ReadDataContext(Ext2Driver &driver, Inode inode, void *buffer,
			uintptr_t first_block, size_t num_blocks,
			util::Callback<void()> callback)
			: driver(driver), inode(inode), buffer(buffer),
			firstBlock(first_block), numBlocks(num_blocks),
			blocksRead(0), callback(callback) { }	


	Ext2Driver &driver;
	Inode inode;
	void *buffer;
	uintptr_t firstBlock;
	size_t numBlocks;
	size_t blocksRead;

	util::Callback<void()> callback;
};

auto doReadData = async::repeatWhile(
	async::lambda([](ReadDataContext &context, util::Callback<void(bool)> callback) {
		callback(context.blocksRead < context.numBlocks);
	}),
	async::lambda([](ReadDataContext &context, util::Callback<void()> callback) {
		uint32_t direct_index = context.firstBlock + context.blocksRead;
		uint32_t block = context.inode.directBlocks[direct_index];

		printf("reading block %d, %d of %d complete!\n", block, context.blocksRead, context.numBlocks);
		parser.getPartition(1).readSectors(block * context.driver.sectorsPerBlock,
				(uint8_t *)context.buffer + context.blocksRead * context.driver.blockSize,
				context.driver.sectorsPerBlock, callback);

		context.blocksRead++;
	})
);

void Ext2Driver::readData(Inode inode, void *buffer, uintptr_t first_block, size_t num_blocks,
			util::Callback<void()> callback) {
	auto on_complete = [] (ReadDataContext &context) {
		context.callback();
	};
	async::run(allocator, doReadData, ReadDataContext(*this, inode,
			buffer, first_block, num_blocks, callback), on_complete);
}

struct DirEntry {
	uint32_t inode;
	uint16_t recordLength;
	uint8_t nameLength;
	uint8_t fileType;
	char name[];
};

Ext2Driver ext2driver;

size_t dirBlocks;
void *dirBuffer;

void test3(void *object) {
	uintptr_t offset = 0;
	while(offset < dirBlocks * ext2driver.blockSize) {
		DirEntry *entry = (DirEntry *)((uintptr_t)dirBuffer + offset);

		printf("File: %.*s\n", entry->nameLength, entry->name);

		offset += entry->recordLength;
	}
}

void test2(void *object, Inode inode) {
	dirBlocks = inode.size / ext2driver.blockSize;
	if((inode.size % ext2driver.blockSize) != 0)
		dirBlocks++;
	dirBuffer = malloc(dirBlocks * ext2driver.blockSize);
	
	ext2driver.readData(inode, dirBuffer, 0, dirBlocks,
			util::Callback<void()>(nullptr, &test3));
}

void test(void *object){
	ext2driver.readInode(2, util::Callback<void(Inode)>(nullptr, &test2));
}

void testExt2Driver(){
	ext2driver.init(util::Callback<void()>(nullptr, &test));
}

void onTableComplete(void *object) {
	testExt2Driver();
}

void testAta() {
	parser.parse(util::Callback<void()>(nullptr, &onTableComplete));
}


// --------------------------------------------------------
// IPC testing code
// --------------------------------------------------------

uint8_t recvBuffer[10];

void onReceive(void *object, HelError error,
		int64_t msg_request, int64_t msg_sequence, size_t length) {
	printf("ok %d %u %s\n", error, length, recvBuffer);
}

void onAccept(void *object, HelError error, HelHandle handle) {
	printf("accept\n");
	
	HEL_CHECK(helSendString(handle, (const uint8_t *)"hello", 6, 1, 1));
}
void onConnect(void *object, HelError error, HelHandle handle) {
	printf("connect\n");
	
	int64_t async_id;
	HEL_CHECK(helSubmitRecvString(handle, eventHub.getHandle(),
			recvBuffer, 10, kHelAnyRequest, kHelAnySequence,
			(uintptr_t)nullptr, (uintptr_t)&onReceive, &async_id));
}

void testIpc() {
	HelHandle socket;

	HelHandle server, client;
	HEL_CHECK(helCreateServer(&server, &client));
	int64_t submit_id, accept_id;
	HEL_CHECK(helSubmitAccept(server, eventHub.getHandle(),
			(uintptr_t)nullptr, (uintptr_t)&onAccept, &submit_id));
	HEL_CHECK(helSubmitConnect(client, eventHub.getHandle(),
			(uintptr_t)nullptr, (uintptr_t)&onConnect, &accept_id));
}

// --------------------------------------------------------
// main
// --------------------------------------------------------

void testScreen() {
	// note: the vga test mode memory is actually 4000 bytes long
	HelHandle screen_memory;
	HEL_CHECK(helAccessPhysical(0xB8000, 0x1000, &screen_memory));

	void *actual_pointer;
	HEL_CHECK(helMapMemory(screen_memory, kHelNullHandle, nullptr, 0x1000,
			kHelMapReadWrite, &actual_pointer));
	
	uint8_t *screen_ptr = (uint8_t *)actual_pointer;
	screen_ptr[0] = 'H';
	screen_ptr[1] = 0x0F;
	asm volatile ( "" : : : "memory" );
}

int main() {
	testAta();
	//testIpc();
	testScreen();

	while(true)
		eventHub.defaultProcessEvents();
}

