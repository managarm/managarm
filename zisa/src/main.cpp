
/*#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

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

struct ParseContext {
	ParseContext(GptParser &parser, util::Callback<void()> callback)
	: parser(parser), callback(callback) { }

	GptParser &parser;
	uint8_t headerBuffer[512];
	uint8_t *tableBuffer;
	util::Callback<void()> callback;
};

void GptParser::parse(util::Callback<void()> callback) {
	auto on_complete = [] (ParseContext &context) {
		context.callback();
	};

	async::run(allocator, doParse, ParseContext(*this, callback), on_complete);
}

GptParser parser;


// --------------------------------------------------------
// Superblock testing code
// --------------------------------------------------------

struct InitContext {
	InitContext(Ext2Driver &driver, util::Callback<void()> callback)
	: driver(driver), callback(callback) { }	

	Ext2Driver &driver;
	uint8_t superblockBuffer[1024];

	util::Callback<void()> callback;
};

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
// main
// --------------------------------------------------------

thread_local int x;

int main() {
	//testAta();
	x = 5;
	printf("Thread local store\n");

//	while(true)
//		eventHub.defaultProcessEvents();
}*/

#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <unordered_map>

#include <frigg/traits.hpp>
#include <frigg/algorithm.hpp>
#include <frigg/atomic.hpp>
#include <frigg/memory.hpp>

#include <hel.h>
#include <hel-syscalls.h>
#include <helx.hpp>
#include <thor.h>

#include "fs.pb.h"

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

helx::EventHub eventHub = helx::EventHub::create();

struct LibcAllocator {
	void *allocate(size_t length) {
		return malloc(length);
	}

	void free(void *pointer) {
		free(pointer);
	}
};

LibcAllocator allocator;

void thread1() {
	helLog("a", 1);
	while(true) {
//		helLog("a", 1);
	}
}

void thread2() {
	helLog("b", 1);
	while(true) {
//		helLog("b", 1);
	}
}

uint8_t stack1[4096];
uint8_t stack2[4096];

#include <unistd.h>
#include <sys/helfd.h>

extern "C" void testSignal() {
	printf("In signal\n");
}

asm ( ".section .text\n"
	".global signalEntry\n"
	"signalEntry:\n"
	"\tcall testSignal\n"
	"\tmov $40, %rdi\n"
	"\tsyscall\n" );

extern "C" void signalEntry();

int main() {
	printf("Hello world\n");

/*	HelHandle handle;
	HEL_CHECK(helCreateSignal((void *)&signalEntry, &handle));
	HEL_CHECK(helRaiseSignal(handle));
	
	printf("After signal\n");

/*	int fd = open("/dev/hw", O_RDONLY);
	assert(fd != -1);

	HelHandle handle;
	int clone_res = helfd_clone(fd, &handle);
	assert(clone_res == 0);

	HelThreadState state;
	HelHandle handle1, handle2;

	state.rip = (uint64_t)&thread1;
	state.rsp = (uint64_t)(stack1 + 4096);
	HEL_CHECK(helCreateThread(kHelNullHandle, kHelNullHandle, &state, 0, &handle1));
	
	state.rip = (uint64_t)&thread2;
	state.rsp = (uint64_t)(stack2 + 4096);
	HEL_CHECK(helCreateThread(kHelNullHandle, kHelNullHandle, &state, 0, &handle1));*/

	HEL_CHECK(helControlKernel(kThorSubDebug, kThorIfDebugMemory, nullptr, nullptr));
}

