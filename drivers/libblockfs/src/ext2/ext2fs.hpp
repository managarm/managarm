
#include <expected>
#include <functional>
#include <string.h>
#include <time.h>
#include <optional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <protocols/fs/file-locks.hpp>

#include <async/oneshot-event.hpp>
#include <async/recurring-event.hpp>
#include <hel.h>

#include <blockfs.hpp>
#include "../common.hpp"
#include "fs.bragi.hpp"
#include "../fs.hpp"

namespace blockfs {
namespace ext2fs {

// --------------------------------------------------------
// On-disk structures
// --------------------------------------------------------

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

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

struct Inode final : BaseInode, std::enable_shared_from_this<Inode> {
	Inode(FileSystem &fs, uint32_t number);

	DiskInode *diskInode();

	// Returns the size of the file in bytes.
	uint64_t fileSize() {
		return diskInode()->size;
	}

	void setFileSize(uint64_t size);

	async::result<frg::expected<protocols::fs::Error, std::optional<DirEntry>>>
	findEntry(std::string name);

	async::result<frg::expected<protocols::fs::Error, DirEntry>> insertEntry(std::string name, int64_t ino, blockfs::FileType type);

	async::result<std::expected<DirEntry, protocols::fs::Error>> link(std::string name, int64_t ino, blockfs::FileType type);
	async::result<frg::expected<protocols::fs::Error>> unlink(std::string name);
	async::result<std::expected<DirEntry, protocols::fs::Error>> mkdir(std::string name);
	async::result<std::expected<DirEntry, protocols::fs::Error>> symlink(std::string name, std::string target);
	async::result<protocols::fs::Error> chmod(int mode);
	async::result<protocols::fs::Error> updateTimes(
		std::optional<timespec> atime,
		std::optional<timespec> mtime,
		std::optional<timespec> ctime);

	FileSystem &fs;

	helix::UniqueDescriptor diskLock;

	// page cache that stores the contents of this file
	HelHandle backingMemory;
	HelHandle frontalMemory;
	helix::Mapping fileMapping;

	helix::BorrowedDescriptor accessMemory() {
		return helix::BorrowedDescriptor{frontalMemory};
	}

	async::result<frg::expected<protocols::fs::Error>>
	ensureBackingBlocks(size_t offset, size_t length);

	async::result<frg::expected<protocols::fs::Error>>
	resizeFile(size_t newSize);

	// Caches indirection blocks reachable from the inode.
	// - Indirection level 1/1 for single indirect blocks.
	// - Indirection level 1/2 for double indirect blocks.
	// - Indirection level 1/3 for triple indirect blocks.
	helix::UniqueDescriptor indirectOrder1;
	// Caches indirection blocks reachable from order 1 blocks.
	// - Indirection level 2/2 for double indirect blocks.
	// - Indirection level 2/3 for triple indirect blocks.
	helix::UniqueDescriptor indirectOrder2;
	// Caches indirection blocks reachable from order 2 blocks.
	// - Indirection level 3/3 for triple indirect blocks.
	helix::UniqueDescriptor indirectOrder3;
};

// --------------------------------------------------------
// FileSystem
// --------------------------------------------------------

struct OpenFile;

struct FileSystem final : BaseFileSystem {
	using Inode = Inode;
	using File = OpenFile;

	FileSystem(BlockDevice *device);

	const protocols::fs::FileOperations *fileOps() override;
	const protocols::fs::NodeOperations *nodeOps() override;

	async::result<void> init();

	async::recurring_event bdgtWriteback;
	async::detached handleBgdtWriteback();

	async::detached manageBlockBitmap(helix::UniqueDescriptor memory);
	async::detached manageInodeBitmap(helix::UniqueDescriptor memory);
	async::detached manageInodeTable(helix::UniqueDescriptor memory);

	std::shared_ptr<BaseInode> accessRoot() override;
	std::shared_ptr<BaseInode> accessInode(uint32_t number) override;
	async::result<std::shared_ptr<BaseInode>> createRegular(int uid, int gid, uint32_t parentIno) override;
	protocols::fs::FsStats getFsStats() override;

	async::result<std::shared_ptr<Inode>> createDirectory();
	async::result<std::shared_ptr<Inode>> createSymlink();

	async::detached initiateInode(std::shared_ptr<Inode> inode);
	async::detached manageFileData(std::shared_ptr<Inode> inode);
	async::detached manageIndirect(std::shared_ptr<Inode> inode, int order,
			helix::UniqueDescriptor memory);

	// Allocate up to num blocks for the given inode.
	// This function does not write back the BGDT, this is the caller's responsibility.
	async::result<std::vector<uint32_t>> allocateBlocks(size_t num, std::optional<uint32_t> ino = std::nullopt);
	async::result<uint32_t> allocateInode(uint32_t parentIno = 0, bool directory = false);

	async::result<void> assignDataBlocks(Inode *inode,
			uint64_t block_offset, size_t num_blocks);

	async::result<void> readDataBlocks(std::shared_ptr<Inode> inode, uint64_t block_offset,
			size_t num_blocks, void *buffer);
	async::result<void> writeDataBlocks(std::shared_ptr<Inode> inode, uint64_t block_offset,
			size_t num_blocks, const void *buffer);

	BlockDevice *device;
	uint16_t inodeSize;
	uint32_t blockShift;
	uint32_t blockSize;
	uint32_t blockPagesShift;
	uint32_t sectorsPerBlock;
	uint32_t numBlockGroups;
	uint32_t blocksPerGroup;
	uint32_t inodesPerGroup;
	uint32_t blocksCount;
	uint32_t inodesCount;
	std::vector<std::byte> blockGroupDescriptorBuffer;
	DiskGroupDesc *bgdt;

	helix::UniqueDescriptor blockBitmap;
	helix::Mapping blockBitmapMapping;
	helix::UniqueDescriptor inodeBitmap;
	helix::Mapping inodeBitmapMapping;
	helix::UniqueDescriptor inodeTable;
	helix::Mapping inodeTableMapping;

	std::unordered_map<uint32_t, std::weak_ptr<Inode>> activeInodes;
};

// --------------------------------------------------------
// File operation closures
// --------------------------------------------------------

struct OpenFile final : BaseFile {
	OpenFile(std::shared_ptr<Inode> inode, bool append)
	: BaseFile{inode, append} { }

	async::result<std::optional<std::string>> readEntries();
};

static_assert(blockfs::Inode<Inode>);
static_assert(blockfs::File<OpenFile>);
static_assert(blockfs::FileSystem<FileSystem>);

} } // namespace blockfs::ext2fs

