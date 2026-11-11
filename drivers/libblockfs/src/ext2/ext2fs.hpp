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

using DiskU16 = arch::endian_scalar<uint16_t, arch::endian::little>;
using DiskU32 = arch::endian_scalar<uint32_t, arch::endian::little>;
using DiskU64 = arch::endian_scalar<uint64_t, arch::endian::little>;

struct ExtentIndex;
struct Extent;

struct ExtentHeader {
	DiskU16 magic;
	DiskU16 entries;
	DiskU16 max;
	DiskU16 depth;
	DiskU32 generation;

	// The entries follow the header; a node holds indices unless its depth is zero.
	Extent *extents();
	ExtentIndex *indices();
};
static_assert(sizeof(ExtentHeader) == 12, "Bad ExtentHeader struct size");

enum {
	EXT4_EXTENT_MAGIC = 0xF30A
};

struct ExtentIndex {
	DiskU32 block;
	DiskU32 leafLow;
	DiskU16 leafHigh;
	DiskU16 unused;

	uint64_t leaf() const {
		return static_cast<uint64_t>(leafLow) | (static_cast<uint64_t>(leafHigh) << 32);
	}
};
static_assert(sizeof(ExtentIndex) == 12, "Bad ExtentIndex struct size");

struct Extent {
	DiskU32 block;
	DiskU16 len;
	DiskU16 startHigh;
	DiskU32 startLow;

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
		DiskU32 direct[12];
		DiskU32 singleIndirect;
		DiskU32 doubleIndirect;
		DiskU32 tripleIndirect;
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
	DiskU32 inodesCount;
	DiskU32 blocksCount;
	DiskU32 rBlocksCount;
	DiskU32 freeBlocksCount;
	DiskU32 freeInodesCount;
	DiskU32 firstDataBlock;
	DiskU32 logBlockSize;
	DiskU32 logFragSize;
	DiskU32 blocksPerGroup;
	DiskU32 fragsPerGroup;
	DiskU32 inodesPerGroup;
	DiskU32 mtime;
	DiskU32 wtime;
	DiskU16 mntCount;
	DiskU16 maxMntCount;
	DiskU16 magic;
	DiskU16 state;
	DiskU16 errors;
	DiskU16 minorRevLevel;
	DiskU32 lastcheck;
	DiskU32 checkinterval;
	DiskU32 creatorOs;
	DiskU32 revLevel;
	DiskU16 defResuid;
	DiskU16 defResgid;
	//-- EXT2_DYNAMIC_REV Specific --
	DiskU32 firstIno;
	DiskU16 inodeSize;
	DiskU16 blockGroupNr;
	DiskU32 featureCompat;
	DiskU32 featureIncompat;
	DiskU32 featureRoCompat;
	uint8_t uuid[16];
	uint8_t volumeName[16];
	uint8_t lastMounted[64];
	DiskU32 algoBitmap;
	//-- Performance Hints --
	uint8_t preallocBlocks;
	uint8_t preallocDirBlocks;
	union {
		DiskU16 alignment;
		DiskU16 reservedGdtBlocks;
	};
	//-- Journaling Support --
	uint8_t journalUuid[16];
	DiskU32 journalInum;
	DiskU32 journalDev;
	DiskU32 lastOrphan;
	//-- Directory Indexing Support --
	DiskU32 hashSeed[4];
	uint8_t defHashVersion;
	uint8_t jnlBackupType;
	// Valid only with EXT4_INCOMPAT_64BIT
	DiskU16 groupDescSize;
	//-- Other options --
	DiskU32 defaultMountOptions;
	DiskU32 firstMetaBg;

	DiskU32 mkfsTime;
	DiskU32 jnlBlocks[17];
	//-- Valid only with EXT4_INCOMPAT_64BIT --
	DiskU32 blocksCountHigh;
	DiskU32 rBlocksCountHigh;
	DiskU32 freeBlocksCountHigh;
	DiskU16 minExtraIsize;
	DiskU16 wantExtraIsize;
	DiskU32 flags;
	DiskU16 s_raid_stride;
	DiskU16 mmpInterval;
	DiskU64 mmpBlock;
	DiskU32 raidStripeWidth;
	uint8_t logGroupsPerFlex;
	uint8_t checksumType;
	uint8_t encryptionLevel;
	uint8_t alignment1;
	DiskU64 kbytesWritten;
	DiskU32 snapshotInum;
	DiskU32 snapshotId;
	DiskU64 snapshotRBlocksCount;
	DiskU32 snapshotList;
	DiskU32 errorCount;
	DiskU32 firstErrorTime;
	DiskU32 firstErrorIno;
	DiskU64 firstErrorBlock;
	uint8_t firstErrorFunc[32];
	DiskU32 firstErrorLine;
	DiskU32 lastErrorTime;
	DiskU32 lastErrorIno;
	DiskU32 lastErrorLine;
	DiskU64 lastErrorBlock;
	uint8_t lastErrorFunc[32];
	uint8_t mountOpts[64];
	DiskU32 usrQuotaInum;
	DiskU32 grpQuotaInum;
	DiskU32 overheadBlocks;
	// Valid only with EXT4_COMPAT_SPARSE_SUPER2
	DiskU32 backupBgs[2];
	uint8_t encryptAlgos[4];
	uint8_t encryptPwSalt[16];
	DiskU32 lpfIno;
	DiskU32 prjQuotaInum;
	DiskU32 checksumSeed;
	uint8_t wtimeHigh;
	uint8_t mtimeHigh;
	uint8_t mkfsTimeHigh;
	uint8_t lastcheckHigh;
	uint8_t firstErrorTimeHigh;
	uint8_t lastErrorTimeHigh;
	uint8_t firstErrorErrcode;
	uint8_t lastErrorErrcode;
	DiskU16 encoding;
	DiskU16 encodingFlags;
	DiskU32 orphanFileInum;
	DiskU32 unused[94];
	DiskU32 checksum;
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
	DiskU32 blockBitmap;
	DiskU32 inodeBitmap;
	DiskU32 inodeTable;
	DiskU16 freeBlocksCount;
	DiskU16 freeInodesCount;
	DiskU16 usedDirsCount;
	DiskU16 flags;
	DiskU32 excludeBitmapLow;
	DiskU16 blockBitmapCsumLow;
	DiskU16 inodeBitmapCsumLow;
	DiskU16 itableUnusedLow;
	DiskU16 checksum;
	//-- Valid only with EXT4_INCOMPAT_64BIT and desc size > 32 --
	DiskU32 blockBitmapHigh;
	DiskU32 inodeBitmapHigh;
	DiskU32 inodeTableHigh;
	DiskU16 freeBlocksCountHigh;
	DiskU16 freeInodesCountHigh;
	DiskU16 usedDirsCountHigh;
	DiskU16 itableUnusedHigh;
	DiskU32 excludeBitmapHigh;
	DiskU16 blockBitmapCsumHigh;
	DiskU16 inodeBitmapCsumHigh;
	DiskU32 unused;
};
static_assert(sizeof(DiskGroupDesc) == 64, "Bad DiskGroupDesc struct size");

struct DiskInode {
	DiskU16 mode;
	DiskU16 uid;
	DiskU32 size;
	DiskU32 atime;
	DiskU32 ctime;
	DiskU32 mtime;
	DiskU32 dtime;
	DiskU16 gid;
	DiskU16 linksCount;
	DiskU32 blocks;
	DiskU32 flags;
	DiskU32 osdl;
	FileData data;
	DiskU32 generation;
	DiskU32 fileAcl;
	DiskU32 dirAcl;
	DiskU32 faddr;
	union {
		uint8_t data[12];

		struct {
			DiskU16 blocksHigh;
			DiskU16 fileAclHigh;
			DiskU16 uidHigh;
			DiskU16 gidHigh;
			DiskU16 checksumLow;
			DiskU16 reserved;
		};
	} osd2;
	DiskU16 extraSize;
	DiskU16 checksumHigh;
	DiskU32 ctimeExtra;
	DiskU32 mtimeExtra;
	DiskU32 atimeExtra;
	DiskU32 crtime;
	DiskU32 crtimeExtra;
	DiskU32 versionHigh;
	DiskU32 projid;
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
	DiskU32 inode;
	DiskU16 recordLength;
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
