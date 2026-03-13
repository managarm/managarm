#pragma once

#include <async/generator.hpp>
#include <async/result.hpp>
#include <map>
#include <print>

#include "../fs.hpp"
#include "blockfs.hpp"
#include "spec.hpp"

namespace {

constexpr bool verboseLogging = false;
constexpr bool debugTreeWalking = false;

}

namespace blockfs::btrfs {

struct DirEntry {
	uint32_t inode;
	FileType fileType;
};

struct Inode final : BaseInode, std::enable_shared_from_this<Inode> {
	friend FileSystem;

	Inode(FileSystem &fs, uint32_t number);

	uint64_t fileSize() { return size_; }

	async::result<protocols::fs::Error> updateTimes(
	    std::optional<::timespec> atime,
	    std::optional<::timespec> mtime,
	    std::optional<::timespec> ctime
	);

	async::result<frg::expected<protocols::fs::Error>> resizeFile(size_t newSize);

	helix::BorrowedDescriptor accessMemory() { return helix::BorrowedDescriptor{frontalMemory}; }

	async::result<frg::expected<protocols::fs::Error, std::optional<DirEntry>>>
	findEntry(std::string name);

	HelHandle backingMemory;
	HelHandle frontalMemory;

	FileSystem &fs_;

private:
	size_t size_;
};

struct OpenFile;

struct BtreePtrLayer {
	LogicalAddress logical;
	key key;
	helix::Mapping mapping;
};

using BtreePtr = std::vector<BtreePtrLayer>;

struct FileSystem final : BaseFileSystem {
	friend struct PhysicalAddress;

	using Inode = Inode;
	using File = OpenFile;
	using DirEntry = DirEntry;

	FileSystem(BlockDevice *device);

	async::result<void> init();
	async::detached manageTree();

	// btrfs tree walking helpers
	async::result<std::optional<std::span<std::byte>>>
	find(LogicalAddress start, key k, BtreePtr &stack);

	async::result<std::optional<std::span<std::byte>>>
	lowerBound(LogicalAddress start, key k, BtreePtr &stack);

	async::result<std::optional<std::span<std::byte>>>
	upperBound(LogicalAddress start, key k, BtreePtr &stack);

	async::result<std::optional<std::span<std::byte>>>
	nextKey(BtreePtr &stack);

	async::result<std::optional<std::span<std::byte>>>
	firstKey(LogicalAddress, BtreePtr &stack);

	async::generator<std::tuple<key, std::span<std::byte>>> traverse(LogicalAddress start);

	const protocols::fs::FileOperations *fileOps() override;
	const protocols::fs::NodeOperations *nodeOps() override;

	std::shared_ptr<BaseInode> accessRoot() override;
	std::shared_ptr<BaseInode> accessInode(uint32_t number) override;
	async::result<std::shared_ptr<BaseInode>>
	createRegular(int uid, int gid, uint32_t parentIno) override;
	protocols::fs::FsStats getFsStats() override;

	async::detached initiateInode(std::shared_ptr<Inode> inode);
	async::detached manageFileData(std::shared_ptr<Inode> inode);

	LogicalAddress fsTreeRoot_;
	uint64_t rootInode_;

private:
	async::result<std::pair<helix::Mapping, helix::LockMemoryView>>
	getBtreeNode(LogicalAddress start);

	BlockDevice *device_;

	superblock superblock_;

	std::unordered_map<uint32_t, std::weak_ptr<Inode>> activeInodes;

	struct CachedChunk {
		LogicalAddress addr;
		uint64_t size;
		chunk_stripe stripe;
	};

	HelHandle treeBackingMemory;
	HelHandle treeFrontalMemory;

	// Maps logical address ranges to physical stripes.
	std::map<uint64_t, CachedChunk> cachedChunks_;
};

struct OpenFile final : BaseFile {
	OpenFile(std::shared_ptr<Inode> inode, bool write, bool read, bool append)
	: BaseFile{inode, write, read, append} {}
};

static_assert(blockfs::Inode<Inode>);
static_assert(blockfs::File<OpenFile>);
static_assert(blockfs::FileSystem<FileSystem>);

} // namespace blockfs::btrfs
