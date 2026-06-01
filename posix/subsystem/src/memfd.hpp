#pragma once

#include <fcntl.h>

#include "file.hpp"
#include "fs.hpp"

struct MemoryFile final : File {
public:
	static void serve(smarter::shared_ptr<MemoryFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	MemoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, bool allowSealing)
	: File{FileKind::unknown,  StructName::get("memfd-file"), mount, link}, _offset{0} {
		if(!allowSealing) {
			_seals = F_SEAL_SEAL;
		}
	}

	void handleClose() override;

	async::result<frg::expected<Error, off_t>> seek(off_t delta, VfsSeek whence) override;
	async::result<frg::expected<protocols::fs::Error>> allocate(int64_t offset, size_t size) override;

	async::result<frg::expected<protocols::fs::Error>> truncate(size_t size) override;

	async::result<frg::expected<protocols::fs::Error, int>> getSeals() override;
	async::result<frg::expected<protocols::fs::Error, int>> addSeals(int seals) override;

	async::result<frg::expected<Error, size_t>>
	writeAll(Process *process, const void *data, size_t length) override;

	async::result<std::expected<size_t, Error>>
	readSome(Process *process, void *data, size_t max_length, async::cancellation_token ct) override;

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

	size_t fileSize() const {
		return _fileSize;
	}

private:
	async::result<frg::expected<Error>> _resizeFile(size_t new_size);

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	uint64_t _offset;

	helix::UniqueDescriptor _memory;
	helix::Mapping _mapping;
	size_t _areaSize = 0;
	size_t _fileSize = 0;
	int _seals = 0;
};

// ----------------------------------------------------------------------------
// MemoryFileLink class.
// ----------------------------------------------------------------------------

struct MemoryFileLink final : FsLink, std::enable_shared_from_this<MemoryFileLink> {
private:
	struct PrivateTag { }; // To tag-dispatch to private methods.

public:
	static std::shared_ptr<MemoryFileLink> makeMemoryFileLink(int mode) {
		return std::make_shared<MemoryFileLink>(PrivateTag{}, mode);
	}

	MemoryFileLink(PrivateTag, int mode)
	: mode_{mode} { }

	void setFile(smarter::shared_ptr<MemoryFile> file) {
		file_ = std::move(file);
	}

public:
	std::shared_ptr<FsNode> getTarget() override {
		return {shared_from_this(), &embeddedNode_};
	}

	std::shared_ptr<FsNode> getOwner() override {
		return nullptr;
	}

	std::string getName() override {
		throw std::runtime_error("MemoryFileLink has no name");
	}

	std::optional<std::string> getProcFsDescription() override {
		return "memory_inode:unimplemented";
	}

private:
	// MemoryFileLink can never be linked into "real" file systems,
	// hence the can only ever be one link per node.
	struct EmbeddedNode final : FsNode {
		EmbeddedNode() : FsNode(getAnonymousSuperblock()) {}

		VfsType getType() override {
			return VfsType::regular;
		}

		async::result<frg::expected<Error, FileStats>> getStats() override {
			auto node = frg::container_of(this, &MemoryFileLink::embeddedNode_);
			FileStats stats{};
			stats.inodeNumber = 1;
			stats.fileSize = node->file_->fileSize();
			stats.numLinks = 1;
			stats.mode = node->mode_;
			stats.uid = 0;
			stats.gid = 0;
			// TODO: Linux returns the current time for all timestamps.
			co_return stats;
		}
	};

	smarter::shared_ptr<MemoryFile> file_;
	int mode_;
	[[no_unique_address]] EmbeddedNode embeddedNode_;
};
