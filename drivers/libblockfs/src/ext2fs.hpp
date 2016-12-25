
#include <time.h>
#include <experimental/optional>
#include <memory>
#include <unordered_map>
#include <vector>

#include <blockfs.hpp>
#include "common.hpp"
#include "cache.hpp"
#include "fs.pb.h"

namespace blockfs {
namespace ext2fs {

// --------------------------------------------------------
// On-disk structures
// --------------------------------------------------------

union FileData {
	struct Blocks {
		uint32_t direct[12];
		uint32_t singleIndirect;
		uint32_t doubleIndirect;
		uint32_t tripleIndirect;
	};

	Blocks blocks;
	uint8_t embedded[60];
};
static_assert(sizeof(FileData) == 60, "Bad FileData struct size");

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
	FileData data;
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

enum {
	EXT2_S_IFMT = 0xF000,
	EXT2_S_IFLNK = 0xA000,
	EXT2_S_IFREG = 0x8000,
	EXT2_S_IFDIR = 0x4000
};

struct DiskDirEntry {
	uint32_t inode;
	uint16_t recordLength;
	uint8_t nameLength;
	uint8_t fileType;
	char name[];
};

enum {
	EXT2_FT_REG_FILE = 1,
	EXT2_FT_DIR = 2,
	EXT2_FT_SYMLINK = 7
};
/*
// --------------------------------------------------------
// DirEntry
// --------------------------------------------------------

struct DirEntry {
	uint32_t inode;
	FileType fileType;
};

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

struct FileSystem;

struct Inode : std::enable_shared_from_this<Inode> {
	Inode(FileSystem &fs, uint32_t number);

	void findEntry(std::string name,
			frigg::CallbackPtr<void(std::experimental::optional<DirEntry>)> callback);

	void onLoadRequest(HelError error, uintptr_t offset, size_t length);

	FileSystem &fs;

	// ext2fs on-disk inode number
	const uint32_t number;
	
	// true if this inode has already been loaded from disk
	bool isReady;
	
	// called when the inode becomes ready
	std::vector<frigg::CallbackPtr<void()>> readyQueue;

	// page cache that stores the contents of this file
	HelHandle backingMemory;
	HelHandle frontalMemory;
	
	// NOTE: The following fields are only meaningful if the isReady is true

	FileType fileType;
	uint64_t fileSize; // file size in bytes
	FileData fileData; // block references / small symlink data

	uint32_t mode; // POSIX file mode
	int numLinks; // number of links to this file
	int uid, gid;
	struct timespec atime, mtime, ctime;
};

// --------------------------------------------------------
// FileSystem
// --------------------------------------------------------

struct FileSystem {
	FileSystem(helx::EventHub &event_hub, BlockDevice *device);

	void init(frigg::CallbackPtr<void()> callback);	

	std::shared_ptr<Inode> accessRoot();
	std::shared_ptr<Inode> accessInode(uint32_t number);

	void readData(std::shared_ptr<Inode> inode, uint64_t block_offset,
			size_t num_blocks, void *buffer, frigg::CallbackPtr<void()> callback);

	struct BlockCache;

	struct BlockCacheEntry {
	friend class BlockCache;
		enum State {
			kStateInitial,
			kStateLoading,
			kStateReady
		};

		BlockCacheEntry(void *buffer);

		void waitUntilReady(frigg::CallbackPtr<void()> callback);

		void *buffer;
	
	private:
		void loadComplete();

		// current state of the cached content
		State state;
	
		// called when the cache entry becomes ready
		std::vector<frigg::CallbackPtr<void()>> readyQueue;
	};
	
	struct BlockCache : util::Cache<uint64_t, BlockCacheEntry> {
		BlockCache(FileSystem &fs);

		Element *allocate() override;
		void initEntry(uint64_t block, BlockCacheEntry *entry) override;
		void finishEntry(BlockCacheEntry *entry) override;

		FileSystem &fs;
	};

	struct InitClosure {
		InitClosure(FileSystem &ext2fs, frigg::CallbackPtr<void()> callback);

		void operator() ();
	
	private:
		void readSuperblock();
		void readBlockGroups();

		FileSystem &ext2fs;
		frigg::CallbackPtr<void()> callback;
		char *superblockBuffer;
	};

	struct ReadInodeClosure {
		ReadInodeClosure(FileSystem &ext2fs, std::shared_ptr<Inode> inode);

		void operator() ();
	
	private:
		void readSector();

		FileSystem &ext2fs;
		std::shared_ptr<Inode> inode;
		char *sectorBuffer;
	};

	struct ReadDataClosure {
		ReadDataClosure(FileSystem &ext2fs, std::shared_ptr<Inode> inode,
				uint64_t block_offset, size_t num_blocks, void *buffer,
				frigg::CallbackPtr<void()> callback);
		
		void operator() ();

	private:
		void inodeReady();
		void readLevel1();
		void readLevel0();
		void readBlock();

		FileSystem &ext2fs;
		std::shared_ptr<Inode> inode;
		uint64_t blockOffset;
		size_t numBlocks;
		void *buffer;
		frigg::CallbackPtr<void()> callback;
		
		size_t blocksRead;
		size_t chunkSize;
		size_t indexLevel1;
		size_t indexLevel0;
		BlockCache::Ref refLevel1;
		BlockCache::Ref refLevel0;
	};

	helx::EventHub &eventHub;
	BlockDevice *device;
	uint16_t inodeSize;
	uint32_t blockSize;
	uint32_t sectorsPerBlock;
	uint32_t numBlockGroups;
	uint32_t inodesPerGroup;
	void *blockGroupDescriptorBuffer;
	
	// caches ext2fs meta data blocks
	BlockCache blockCache;

	std::unordered_map<uint32_t, std::weak_ptr<Inode>> activeInodes;
};

// --------------------------------------------------------
// File operation closures
// --------------------------------------------------------

struct OpenFile {
	OpenFile(std::shared_ptr<Inode> inode);

	std::shared_ptr<Inode> inode;
	uint64_t offset;
};

struct Client {
	Client(helx::EventHub &event_hub, FileSystem &fs);

	void init(frigg::CallbackPtr<void()> callback);

private:
	struct ObjectHandler : public bragi_mbus::ObjectHandler {
		ObjectHandler(Client &client);
		
		// inherited from bragi_mbus::ObjectHandler
		void requireIf(bragi_mbus::ObjectId object_id,
				frigg::CallbackPtr<void(HelHandle)> callback) override;

		Client &client;
	};

	struct InitClosure {
		InitClosure(Client &client, frigg::CallbackPtr<void()> callback);

		void operator() ();

	private:
		void connected();
		void registered(bragi_mbus::ObjectId object_id);

		Client &client;
		frigg::CallbackPtr<void()> callback;
	};
	
	helx::EventHub &eventHub;
	FileSystem &fs;
	ObjectHandler objectHandler;
	bragi_mbus::Connection mbusConnection;
};

struct Connection {
	Connection(helx::EventHub &event_hub, FileSystem &fs, helx::Pipe pipe);

	void operator() ();

	FileSystem &getFs();
	helx::Pipe &getPipe();

	int attachOpenFile(OpenFile *handle);
	OpenFile *getOpenFile(int handle);

private:
	void recvRequest(HelError error, int64_t msg_request, int64_t msg_seq, size_t length);

	helx::EventHub &eventHub;
	FileSystem &fs;
	helx::Pipe pipe;

	std::unordered_map<int, OpenFile *> fileHandles;
	int nextHandle;
	uint8_t buffer[128];
};

struct OpenClosure {
	OpenClosure(Connection &connection, int64_t response_id,
			managarm::fs::CntRequest request);

	void operator() ();

	void processSegment();
	void foundEntry(std::experimental::optional<DirEntry> number);

	Connection &connection;
	int64_t responseId;
	managarm::fs::CntRequest request;

	std::shared_ptr<Inode> directory;
	std::string tailPath;
};

struct ReadClosure {
	ReadClosure(Connection &connection, int64_t response_id,
			managarm::fs::CntRequest request);

	void operator() ();

	void inodeReady();
	void lockedMemory();

	Connection &connection;
	int64_t responseId;
	managarm::fs::CntRequest request;

	OpenFile *openFile;
};*/

} } // namespace blockfs::ext2fs

