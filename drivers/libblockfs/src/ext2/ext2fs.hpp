#pragma once

#include <assert.h>
#include <arch/endian.hpp>
#include <expected>
#include <functional>
#include <string.h>
#include <time.h>
#include <optional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <protocols/fs/file-locks.hpp>

#include <async/oneshot-event.hpp>
#include <async/sequenced-event.hpp>
#include <hel.h>

#include <blockfs.hpp>
#include "../common.hpp"
#include "fs.bragi.hpp"
#include "../fs.hpp"
#include "../metadata-cache.hpp"
#include "../swap.hpp"
#include "block-map-cache.hpp"

namespace blockfs {
namespace ext2fs {

// --------------------------------------------------------
// On-disk structures
// --------------------------------------------------------

using FlockManager = protocols::fs::FlockManager;
using Flock = protocols::fs::Flock;

struct ExtentIndex;
struct Extent;

struct ExtentHeader {
	arch::le_u16 magic;
	arch::le_u16 entries;
	arch::le_u16 max;
	arch::le_u16 depth;
	arch::le_u32 generation;

	// The entries follow the header; a node holds indices unless its depth is zero.
	Extent *extents();
	ExtentIndex *indices();
};
static_assert(sizeof(ExtentHeader) == 12, "Bad ExtentHeader struct size");

enum {
	EXT4_EXTENT_MAGIC = 0xF30A
};

struct ExtentIndex {
	arch::le_u32 block;
	arch::le_u32 leafLow;
	arch::le_u16 leafHigh;
	arch::le_u16 unused;

	uint64_t leaf() const {
		return static_cast<uint64_t>(leafLow) | (static_cast<uint64_t>(leafHigh) << 32);
	}
};
static_assert(sizeof(ExtentIndex) == 12, "Bad ExtentIndex struct size");

struct Extent {
	arch::le_u32 block;
	arch::le_u16 len;
	arch::le_u16 startHigh;
	arch::le_u32 startLow;

	uint64_t start() const {
		return static_cast<uint64_t>(startLow) | (static_cast<uint64_t>(startHigh) << 32);
	}
};
static_assert(sizeof(Extent) == 12, "Bad Extent struct size");

inline Extent *ExtentHeader::extents() {
	return reinterpret_cast<Extent *>(this + 1);
}

inline ExtentIndex *ExtentHeader::indices() {
	return reinterpret_cast<ExtentIndex *>(this + 1);
}

union FileData {
	struct Blocks {
		arch::le_u32 direct[12];
		arch::le_u32 singleIndirect;
		arch::le_u32 doubleIndirect;
		arch::le_u32 tripleIndirect;
	};

	struct Extents {
		ExtentHeader hdr;
		Extent extents[4];
	};

	Blocks blocks;
	Extents extents;
	uint8_t embedded[60];
};
static_assert(sizeof(FileData) == 60, "Bad FileData struct size");

struct DiskSuperblock {
	arch::le_u32 inodesCount;
	arch::le_u32 blocksCount;
	arch::le_u32 rBlocksCount;
	arch::le_u32 freeBlocksCount;
	arch::le_u32 freeInodesCount;
	arch::le_u32 firstDataBlock;
	arch::le_u32 logBlockSize;
	arch::le_u32 logFragSize;
	arch::le_u32 blocksPerGroup;
	arch::le_u32 fragsPerGroup;
	arch::le_u32 inodesPerGroup;
	arch::le_u32 mtime;
	arch::le_u32 wtime;
	arch::le_u16 mntCount;
	arch::le_u16 maxMntCount;
	arch::le_u16 magic;
	arch::le_u16 state;
	arch::le_u16 errors;
	arch::le_u16 minorRevLevel;
	arch::le_u32 lastcheck;
	arch::le_u32 checkinterval;
	arch::le_u32 creatorOs;
	arch::le_u32 revLevel;
	arch::le_u16 defResuid;
	arch::le_u16 defResgid;
	//-- EXT2_DYNAMIC_REV Specific --
	arch::le_u32 firstIno;
	arch::le_u16 inodeSize;
	arch::le_u16 blockGroupNr;
	arch::le_u32 featureCompat;
	arch::le_u32 featureIncompat;
	arch::le_u32 featureRoCompat;
	uint8_t uuid[16];
	uint8_t volumeName[16];
	uint8_t lastMounted[64];
	arch::le_u32 algoBitmap;
	//-- Performance Hints --
	uint8_t preallocBlocks;
	uint8_t preallocDirBlocks;
	union {
		arch::le_u16 alignment;
		arch::le_u16 reservedGdtBlocks;
	};
	//-- Journaling Support --
	uint8_t journalUuid[16];
	arch::le_u32 journalInum;
	arch::le_u32 journalDev;
	arch::le_u32 lastOrphan;
	//-- Directory Indexing Support --
	arch::le_u32 hashSeed[4];
	uint8_t defHashVersion;
	uint8_t jnlBackupType;
	// Valid only with EXT4_INCOMPAT_64BIT
	arch::le_u16 groupDescSize;
	//-- Other options --
	arch::le_u32 defaultMountOptions;
	arch::le_u32 firstMetaBg;

	arch::le_u32 mkfsTime;
	arch::le_u32 jnlBlocks[17];
	//-- Valid only with EXT4_INCOMPAT_64BIT --
	arch::le_u32 blocksCountHigh;
	arch::le_u32 rBlocksCountHigh;
	arch::le_u32 freeBlocksCountHigh;
	arch::le_u16 minExtraIsize;
	arch::le_u16 wantExtraIsize;
	arch::le_u32 flags;
	arch::le_u16 s_raid_stride;
	arch::le_u16 mmpInterval;
	arch::le_u64 mmpBlock;
	arch::le_u32 raidStripeWidth;
	uint8_t logGroupsPerFlex;
	uint8_t checksumType;
	uint8_t encryptionLevel;
	uint8_t alignment1;
	arch::le_u64 kbytesWritten;
	arch::le_u32 snapshotInum;
	arch::le_u32 snapshotId;
	arch::le_u64 snapshotRBlocksCount;
	arch::le_u32 snapshotList;
	arch::le_u32 errorCount;
	arch::le_u32 firstErrorTime;
	arch::le_u32 firstErrorIno;
	arch::le_u64 firstErrorBlock;
	uint8_t firstErrorFunc[32];
	arch::le_u32 firstErrorLine;
	arch::le_u32 lastErrorTime;
	arch::le_u32 lastErrorIno;
	arch::le_u32 lastErrorLine;
	arch::le_u64 lastErrorBlock;
	uint8_t lastErrorFunc[32];
	uint8_t mountOpts[64];
	arch::le_u32 usrQuotaInum;
	arch::le_u32 grpQuotaInum;
	arch::le_u32 overheadBlocks;
	// Valid only with EXT4_COMPAT_SPARSE_SUPER2
	arch::le_u32 backupBgs[2];
	uint8_t encryptAlgos[4];
	uint8_t encryptPwSalt[16];
	arch::le_u32 lpfIno;
	arch::le_u32 prjQuotaInum;
	arch::le_u32 checksumSeed;
	uint8_t wtimeHigh;
	uint8_t mtimeHigh;
	uint8_t mkfsTimeHigh;
	uint8_t lastcheckHigh;
	uint8_t firstErrorTimeHigh;
	uint8_t lastErrorTimeHigh;
	uint8_t firstErrorErrcode;
	uint8_t lastErrorErrcode;
	arch::le_u16 encoding;
	arch::le_u16 encodingFlags;
	arch::le_u32 orphanFileInum;
	arch::le_u32 unused[94];
	arch::le_u32 checksum;
};
static_assert(sizeof(DiskSuperblock) == 1024, "Bad DiskSuperblock struct size");

enum {
	EXT4_COMPAT_HAS_JOURNAL = 0x4
};

enum {
	EXT4_INCOMPAT_EXTENTS = 0x40,
	EXT4_INCOMPAT_64BIT = 0x80,
	EXT4_INCOMPAT_FLEX_BG = 0x200,
	EXT4_INCOMPAT_CSUM_SEED = 0x2000,
	EXT4_INCOMPAT_LARGEDIR = 0x4000
};

enum {
	EXT4_RO_COMPAT_METADATA_CSUM = 0x400
};

struct DiskGroupDesc {
	arch::le_u32 blockBitmap;
	arch::le_u32 inodeBitmap;
	arch::le_u32 inodeTable;
	arch::le_u16 freeBlocksCount;
	arch::le_u16 freeInodesCount;
	arch::le_u16 usedDirsCount;
	arch::le_u16 flags;
	arch::le_u32 excludeBitmapLow;
	arch::le_u16 blockBitmapCsumLow;
	arch::le_u16 inodeBitmapCsumLow;
	arch::le_u16 itableUnusedLow;
	arch::le_u16 checksum;
	//-- Valid only with EXT4_INCOMPAT_64BIT and desc size > 32 --
	arch::le_u32 blockBitmapHigh;
	arch::le_u32 inodeBitmapHigh;
	arch::le_u32 inodeTableHigh;
	arch::le_u16 freeBlocksCountHigh;
	arch::le_u16 freeInodesCountHigh;
	arch::le_u16 usedDirsCountHigh;
	arch::le_u16 itableUnusedHigh;
	arch::le_u32 excludeBitmapHigh;
	arch::le_u16 blockBitmapCsumHigh;
	arch::le_u16 inodeBitmapCsumHigh;
	arch::le_u32 unused;
};
static_assert(sizeof(DiskGroupDesc) == 64, "Bad DiskGroupDesc struct size");

struct DiskInode {
	arch::le_u16 mode;
	arch::le_u16 uid;
	arch::le_u32 size;
	arch::le_u32 atime;
	arch::le_u32 ctime;
	arch::le_u32 mtime;
	arch::le_u32 dtime;
	arch::le_u16 gid;
	arch::le_u16 linksCount;
	arch::le_u32 blocks;
	arch::le_u32 flags;
	arch::le_u32 osdl;
	FileData data;
	arch::le_u32 generation;
	arch::le_u32 fileAcl;
	arch::le_u32 dirAcl;
	arch::le_u32 faddr;
	union {
		uint8_t data[12];

		struct {
			arch::le_u16 blocksHigh;
			arch::le_u16 fileAclHigh;
			arch::le_u16 uidHigh;
			arch::le_u16 gidHigh;
			arch::le_u16 checksumLow;
			arch::le_u16 reserved;
		};
	} osd2;
	arch::le_u16 extraSize;
	arch::le_u16 checksumHigh;
	arch::le_u32 ctimeExtra;
	arch::le_u32 mtimeExtra;
	arch::le_u32 atimeExtra;
	arch::le_u32 crtime;
	arch::le_u32 crtimeExtra;
	arch::le_u32 versionHigh;
	arch::le_u32 projid;
};
static_assert(sizeof(DiskInode) == 160, "Bad DiskInode struct size");

enum {
	EXT2_ROOT_INO = 2
};

enum {
	EXT2_S_IFMT = 0xF000,
	EXT2_S_IFLNK = 0xA000,
	EXT2_S_IFREG = 0x8000,
	EXT2_S_IFDIR = 0x4000
};

enum {
	EXT4_INDEX_FL = 0x1000,
	EXT4_EXTENTS_FL = 0x80000,
	EXT4_INLINE_DATA_FL = 0x10000000
};

struct DiskDirEntry {
	arch::le_u32 inode;
	arch::le_u16 recordLength;
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
// BlockRange
// --------------------------------------------------------

// Describes a mapping from per-file blocks to on-disk blocks.
struct BlockRange {
	// Offset of the range within the file.
	uint64_t relativeStartBlock;
	// Block number on the file system.
	// Only meaningful if !hole.
	uint64_t absoluteStartBlock;
	// Number of blocks.
	uint64_t size;
	// Whether the range represents a hole or not.
	bool hole;
};

// --------------------------------------------------------
// Inode
// --------------------------------------------------------

struct FileSystem;

struct Inode final : BaseInode, std::enable_shared_from_this<Inode> {
	Inode(FileSystem &fs, uint32_t number);

	DiskInode *diskInode();

	arch::contiguous_pool *getSwapPool();
	async::result<uint64_t> getSwapSize();

	async::result<std::expected<void, Error>> enableSwap();

	async::result<std::expected<void, Error>>
	readSwap(uint64_t offset, arch::dma_buffer_view);

	async::result<std::expected<void, Error>>
	writeSwap(uint64_t offset, arch::dma_buffer_view);

	// Returns the size of the file in bytes.
	// Callers must hold inodeMutex (shared).
	uint64_t fileSize() {
		return diskInode()->size;
	}

	// Callers must hold inodeMutex (exclusive).
	void setFileSize(uint64_t size);

	// Callers must hold inodeMutex (shared).
	async::result<frg::expected<protocols::fs::Error, std::optional<DirEntry>>>
	findEntry(std::string name);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (exclusive), plus the target inode's inodeMutex (exclusive).
	async::result<frg::expected<protocols::fs::Error, DirEntry>> insertEntry(std::string name, int64_t ino, blockfs::FileType type);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (exclusive), plus the target inode's inodeMutex (exclusive).
	async::result<frg::expected<protocols::fs::Error>> removeEntry(std::string name);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (shared).
	async::result<std::expected<bool, protocols::fs::Error>> isDirectoryEmpty();

	// Repoints the ".." entry of this directory at a new parent inode.
	// Callers must hold topologyMutex (exclusive).
	// Callers must hold inodeMutex (exclusive).
	async::result<frg::expected<protocols::fs::Error>> updateDotDot(uint32_t parent);

	// Returns whether this directory is `ino` itself or one of its descendants,
	// found by walking the ".." chain up to the filesystem root.
	// Callers must hold topologyMutex (exclusive).
	async::result<frg::expected<protocols::fs::Error, bool>> isSubdirectoryOf(uint32_t ino);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (exclusive), plus the target inode's inodeMutex (exclusive).
	async::result<std::expected<DirEntry, protocols::fs::Error>> link(std::string name, int64_t ino, blockfs::FileType type);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (exclusive).
	async::result<std::expected<DirEntry, protocols::fs::Error>> mkdir(std::string name, uid_t uid, gid_t gid, mode_t mode);

	// Callers must hold topologyMutex (shared or exclusive).
	// Callers must hold inodeMutex (exclusive).
	async::result<std::expected<DirEntry, protocols::fs::Error>> symlink(std::string name, std::string target);

	// Callers must hold inodeMutex (exclusive).
	async::result<protocols::fs::Error> chmod(int mode);

	// Callers must hold inodeMutex (exclusive).
	async::result<protocols::fs::Error> chown(std::optional<uid_t> uid, std::optional<gid_t> gid);

	// Callers must hold inodeMutex (exclusive).
	async::result<protocols::fs::Error> updateTimes(
		std::optional<timespec> atime,
		std::optional<timespec> mtime,
		std::optional<timespec> ctime);

	FileSystem &fs;

	std::atomic_bool isSwapInode = false;

	MetadataCache::BlockWindow diskInodeWindow;
	size_t diskInodeOffset = 0;

	// Serializes access to this inode's mapping from file offsets to filesystem blocks.
	// blockMapMutex MUST be taken for all operations that access:
	// - the direct pointers in the inode
	// - the single/double/triple indirect blocks
	// Mutations of the block map require the mutex exclusively. Lookups only require it shared.
	// Ordered after inodeMutex.
	async::shared_mutex blockMapMutex;

	// Caches the runs of this inode's block map that were already resolved.
	// Consistent with disk state while blockMapMutex is held (shared or exclusive),
	// but both shared and exclusive locks of blockMapMutex can fill the cache.
	BlockMapCache blockMapCache;

	// page cache that stores the contents of this file
	HelHandle backingMemory;
	HelHandle frontalMemory;
	helix::Mapping fileMapping;

	helix::BorrowedDescriptor accessMemory() {
		return helix::BorrowedDescriptor{frontalMemory};
	}

	// Callers must hold inodeMutex (exclusive).
	async::result<frg::expected<protocols::fs::Error>>
	resizeFile(size_t newSize);

	bool usesExtents;
};

// --------------------------------------------------------
// BlockGroupDescriptorTable
// --------------------------------------------------------

struct BlockGroupDescriptorTable {
	inline void init(std::vector<MetadataCache::BlockWindow> blocks,
			uint32_t blockShift, uint16_t descriptorSize) {
		blocks_ = std::move(blocks);
		blockShift_ = blockShift;
		blockSizeMask_ = (size_t{1} << blockShift_) - 1;

		assert(descriptorSize && !((size_t{1} << blockShift_) % descriptorSize));
		descriptorSize_ = descriptorSize;
	}

	inline DiskGroupDesc& operator[](size_t index) {
		auto offset = index * descriptorSize_;
		auto ptr = static_cast<std::byte *>(blocks_[offset >> blockShift_].get());
		return *reinterpret_cast<DiskGroupDesc *>(ptr + (offset & blockSizeMask_));
	}

	// Call after updating a descriptor through its long-lived cache window.
	void markDirty(size_t index) {
		blocks_[(index * descriptorSize_) >> blockShift_].markDirty();
	}

	uint16_t descriptorSize() const {
		return descriptorSize_;
	}

private:
	std::vector<MetadataCache::BlockWindow> blocks_;
	uint32_t blockShift_;
	size_t blockSizeMask_;
	uint16_t descriptorSize_;
};

// --------------------------------------------------------
// FileSystem
// --------------------------------------------------------

struct OpenFile;

struct FileSystem final : BaseFileSystem {
	using Inode = Inode;
	using File = OpenFile;
	using DirEntry = DirEntry;

	FileSystem(BlockDevice *device);

	const protocols::fs::FileOperations *fileOps() override;
	const protocols::fs::NodeOperations *nodeOps() override;

	async::result<std::expected<void, protocols::fs::Error>> init();

	std::shared_ptr<BaseInode> accessRoot() override;
	std::shared_ptr<BaseInode> accessInode(uint32_t number) override;
	async::result<std::shared_ptr<BaseInode>> createRegular(int uid, int gid, uint32_t parentIno) override;
	protocols::fs::FsStats getFsStats() override;
	async::result<protocols::fs::Error>
	synchronize(protocols::fs::SynchronizeFlags flags) override;
	async::result<protocols::fs::Error> synchronize(std::shared_ptr<Inode> inode,
			protocols::fs::SynchronizeFlags flags);

	async::result<std::shared_ptr<Inode>> createDirectory();
	async::result<std::shared_ptr<Inode>> createSymlink();

	// Returns the block containing the given on-disk inode and the offset within that block.
	std::pair<uint64_t, size_t> locateDiskInode(uint32_t number);

	async::result<void> initiateInode(std::shared_ptr<Inode> inode);
	async::result<void> manageFileData(std::shared_ptr<Inode> inode);
	async::result<void> serviceFileData(std::shared_ptr<Inode> inode,
			int type, uintptr_t offset, size_t length);

	// Allocate up to num blocks for the given inode.
	async::result<std::vector<uint32_t>> allocateBlocks(size_t num, std::optional<uint32_t> ino = std::nullopt);

	// Release the given blocks back to the block bitmaps.
	async::result<void> freeBlocks(std::vector<uint32_t> blocks);

	async::result<uint32_t> allocateInode(uint32_t parentIno = 0, bool directory = false);

	// Callers must hold inode->blockMapMutex (exclusive).
	async::result<void> assignDataBlocks(Inode *inode,
			uint64_t block_offset, size_t num_blocks);

	// Releases the blocks backing the file blocks at or after firstBlock,
	// including the indirection metadata that becomes unreachable.
	// Callers must hold inode->blockMapMutex (exclusive).
	async::result<void> freeDataBlocks(Inode *inode, uint64_t firstBlock);

	// Resolves a range of file blocks to runs of disk blocks and holes.
	// Callers must hold inode->blockMapMutex (shared or exclusive).
	async::result<std::vector<BlockRange>> lookupBlocks(Inode *inode,
			uint64_t block_offset, size_t num_blocks);

	// Resolves a range of file blocks by walking the on-disk block map.
	// Callers must hold inode->blockMapMutex (shared or exclusive).
	async::result<std::vector<BlockRange>> lookupBlocksOnDisk(Inode *inode,
			uint64_t block_offset, size_t num_blocks);

	// Callers must hold inode->blockMapMutex.
	async::result<void> readDataBlocks(const std::vector<BlockRange> &ranges,
			arch::dma_buffer_view buf);

	// Callers must hold inode->blockMapMutex.
	async::result<void> writeDataBlocks(const std::vector<BlockRange> &ranges,
			arch::dma_buffer_view buf);
	async::result<void> synchronizeFileData(Inode *inode);
	async::result<void> synchronizeMetadata();


	// Callers must hold inode->blockMapMutex (shared or exclusive).
	async::result<std::vector<BlockRange>> lookupBlocksUsingExtent(Inode *inode,
			uint64_t block_offset, size_t num_blocks);

	// Callers must hold inode->blockMapMutex (exclusive).
	async::result<void> assignDataBlocksUsingExtents(Inode *inode,
			uint64_t block_offset, size_t num_blocks);

	// Callers must hold inode->blockMapMutex (exclusive).
	async::result<void> freeDataBlocksUsingExtents(Inode *inode, uint64_t firstBlock);

	// Removes all extents at or after fromBlock from the subtree rooted at hdr,
	// freeing their disk blocks and the tree nodes that become empty.
	// block is the disk block backing hdr, or nullopt for the inode-embedded root.
	// Returns whether the node itself ended up empty.
	// Callers must hold inode->blockMapMutex (exclusive).
	async::result<bool> removeExtentsFrom(Inode *inode, ExtentHeader *hdr,
			std::optional<uint64_t> block, uint64_t fromBlock, size_t &numFreed);

	// Locks a metadata block and returns a window into it.
	async::result<MetadataCache::BlockWindow> accessMetadata(uint64_t block, bool writable) {
		return metadataCacheFor(block).access(block, writable);
	}

	// Reads bytes from a metadata block without mapping it.
	async::result<void> readMetadata(uint64_t block, size_t offset, size_t length, void *buffer) {
		return metadataCacheFor(block).read(block, offset, length, buffer);
	}

	// Discard a cached metadata block without writeback.
	// See MetadataCache::forget() for its preconditions.
	async::result<void> forgetMetadata(uint64_t block) {
		return metadataCacheFor(block).forget(block);
	}

	MetadataCache &metadataCacheFor(uint64_t block) {
		auto index = block / blocksPerGroup;
		assert(index < metadataCaches.size());
		return *metadataCaches[index];
	}

	BlockDevice *device;
	uint16_t inodeSize;
	uint32_t blockShift;
	uint32_t blockSize;
	uint32_t sectorsPerBlock;
	uint32_t numBlockGroups;
	uint32_t blocksPerGroup;
	uint32_t inodesPerGroup;
	uint32_t blocksCount;
	uint32_t inodesCount;
	uint8_t uuid[16];

	// We use one cache of metadata blocks per block group.
	// This allows multiple block groups to be served in parallel on helix::DispatcherPool.
	std::vector<std::unique_ptr<MetadataCache>> metadataCaches;

	// Mutable fields are protected by allocationMutex. These are:
	// - freeBlocksCount
	// - freeInodesCount
	// - usedDirsCount
	// - checksum
	// - blockBitmapCsumLow
	// - inodeBitmapCsumLow
	// - blockBitmapCsumHigh
	// - inodeBitmapCsumHigh
	// All other fields are immutable.
	// Descriptors are pinned in the metadata cache for the filesystem lifetime.
	BlockGroupDescriptorTable bgdt;

	uint32_t metadataChecksumSeed;
	bool is64Bit;
	bool usesExtents;
	bool metadataChecksum;
	bool bgdtChecksum;

	std::mutex activeInodesMutex;

	// Serializes block/inode allocation and BGDT modifications.
	async::mutex allocationMutex;

	// Protected by activeInodesMutex.
	std::unordered_map<uint32_t, std::weak_ptr<Inode>> activeInodes;

	arch::contiguous_pool *pool;
};

// --------------------------------------------------------
// File operation closures
// --------------------------------------------------------

struct OpenFile final : BaseFile {
	OpenFile(std::shared_ptr<Inode> inode, bool write, bool read, bool append)
	: BaseFile{inode, write, read, append} { }

	// Callers must hold BaseFile::mutex.
	// Callers must hold the inode's inodeMutex (shared).
	async::result<std::expected<protocols::fs::ReadEntriesResult, managarm::fs::Errors>> readEntries();
};

static_assert(blockfs::Inode<Inode>);
static_assert(blockfs::File<OpenFile>);
static_assert(blockfs::FileSystem<FileSystem>);

} } // namespace blockfs::ext2fs
