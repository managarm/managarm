#pragma once

#include <fcntl.h>

#include "file.hpp"

struct MemoryFile final : File {
public:
	static void serve(smarter::shared_ptr<MemoryFile> file) {
		helix::UniqueLane lane;
		std::tie(lane, file->_passthrough) = helix::createStream();
		async::detach(protocols::fs::servePassthrough(std::move(lane),
				file, &fileOperations, file->_cancelServe));
	}

	MemoryFile(std::shared_ptr<MountView> mount, std::shared_ptr<FsLink> link, bool allowSealing)
	: File{StructName::get("memfd-file"), mount, link}, _offset{0} {
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

	FutureMaybe<helix::UniqueDescriptor> accessMemory() override;

	helix::BorrowedDescriptor getPassthroughLane() override {
		return _passthrough;
	}

private:
	void _resizeFile(size_t new_size);

	helix::UniqueLane _passthrough;
	async::cancellation_event _cancelServe;

	uint64_t _offset;

	helix::UniqueDescriptor _memory;
	helix::Mapping _mapping;
	size_t _areaSize;
	size_t _fileSize;
	int _seals;
};
